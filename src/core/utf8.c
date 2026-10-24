/*
 * Liu - UTF-8 implementation
 */
#include "utf8.h"
#include "core/utf8_marks.h"
#include "core/utf8_emoji.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <stdatomic.h>

u32 utf8_decode(const u8 *buf, usize len, u32 *codepoint) {
    if (!buf || !codepoint || len == 0) return 0;

    u8 b0 = buf[0];

    if (b0 < 0x80) {
        *codepoint = b0;
        return 1;
    }
    if ((b0 & 0xE0) == 0xC0) {
        if (len < 2 || !utf8_is_continuation(buf[1])) return 0;
        *codepoint = ((u32)(b0 & 0x1F) << 6) | (buf[1] & 0x3F);
        if (*codepoint < 0x80) return 0; /* overlong */
        return 2;
    }
    if ((b0 & 0xF0) == 0xE0) {
        if (len < 3 || !utf8_is_continuation(buf[1]) || !utf8_is_continuation(buf[2])) return 0;
        *codepoint = ((u32)(b0 & 0x0F) << 12) | ((u32)(buf[1] & 0x3F) << 6) | (buf[2] & 0x3F);
        if (*codepoint < 0x800) return 0;
        if (*codepoint >= 0xD800 && *codepoint <= 0xDFFF) return 0; /* surrogate */
        return 3;
    }
    if ((b0 & 0xF8) == 0xF0) {
        if (len < 4 || !utf8_is_continuation(buf[1]) ||
            !utf8_is_continuation(buf[2]) || !utf8_is_continuation(buf[3])) return 0;
        *codepoint = ((u32)(b0 & 0x07) << 18) | ((u32)(buf[1] & 0x3F) << 12) |
                     ((u32)(buf[2] & 0x3F) << 6) | (buf[3] & 0x3F);
        if (*codepoint < 0x10000 || *codepoint > 0x10FFFF) return 0;
        return 4;
    }
    return 0;
}

u32 utf8_encode(u32 cp, u8 *out) {
    if (!out) return 0;
    if (cp < 0x80) {
        out[0] = (u8)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = 0xC0 | (u8)(cp >> 6);
        out[1] = 0x80 | (u8)(cp & 0x3F);
        return 2;
    }
    if (cp < 0x10000) {
        if (cp >= 0xD800 && cp <= 0xDFFF) return 0;
        out[0] = 0xE0 | (u8)(cp >> 12);
        out[1] = 0x80 | (u8)((cp >> 6) & 0x3F);
        out[2] = 0x80 | (u8)(cp & 0x3F);
        return 3;
    }
    if (cp <= 0x10FFFF) {
        out[0] = 0xF0 | (u8)(cp >> 18);
        out[1] = 0x80 | (u8)((cp >> 12) & 0x3F);
        out[2] = 0x80 | (u8)((cp >> 6) & 0x3F);
        out[3] = 0x80 | (u8)(cp & 0x3F);
        return 4;
    }
    return 0;
}

usize utf8_len(const u8 *buf, usize byte_len) {
    if (!buf) return 0;
    usize count = 0;
    const u8 *end = buf + byte_len;
    while (buf < end) {
        buf = utf8_next(buf, end);
        if (!buf) break;
        count++;
    }
    return count;
}

bool utf8_validate_scalar(const u8 *buf, usize len) {
    if (!buf) return len == 0;
    const u8 *end = buf + len;
    while (buf < end) {
        u32 cp;
        u32 consumed = utf8_decode(buf, (usize)(end - buf), &cp);
        if (consumed == 0) return false;
        buf += consumed;
    }
    return true;
}

#ifndef USE_ASM

bool utf8_validate(const u8 *buf, usize len) {
    return utf8_validate_scalar(buf, len);
}

#endif /* !USE_ASM */

const u8 *utf8_next(const u8 *buf, const u8 *end) {
    if (!buf || !end || buf >= end) return NULL;
    u32 cp;
    u32 n = utf8_decode(buf, (usize)(end - buf), &cp);
    if (n == 0) return NULL;
    return buf + n;
}

/* =========================================================================
 * Grapheme cluster table — see the banner in utf8.h for why cells store a
 * handle instead of a wider Cell.
 *
 * Append-only and interned, so the same emoji typed a thousand times costs one
 * entry and a handle never dangles: a cluster scrolled out of the ring buffer
 * stays valid, and the next occurrence resolves to the same handle. Writes
 * happen on the parser thread only; readers (render, copy) never mutate.
 * ========================================================================= */

typedef struct {
    u32 cps[UTF8_CLUSTER_MAX];
    u32 n;
} Cluster;

/* Storage is PAGED: 1024 entries (68 KiB) allocated when the first entry that
 * lands in them is interned, and a page is never moved, so a pointer from
 * utf8_cluster_lookup() stays valid for the life of the process. Handles live
 * in compressed scrollback, sleep snapshots, glyph cache keys, selections and
 * search hits, so nothing can be reclaimed: the table grows on demand up to a
 * fixed ceiling and counts every refusal.
 *
 * The ceiling keeps a handle (bit 30 | index) below font.c's sized-key cluster
 * range, which has room for 0xF0000 of them. */
#define CLUSTER_PAGE_SHIFT  10u
#define CLUSTER_PAGE_SIZE   (1u << CLUSTER_PAGE_SHIFT)
#define CLUSTER_MAX_COUNT   65536u
#define CLUSTER_PAGES       (CLUSTER_MAX_COUNT / CLUSTER_PAGE_SIZE)
#define CLUSTER_HASH_START  4096u        /* power of two; doubles at half load */

/* Text measuring runs on workers too (the diff parse, segmenting), so interning
 * is serialised. Lookups are lock-free: an entry is written in full BEFORE the
 * count that makes it visible is published, and is immutable afterwards. */
static pthread_mutex_t     g_cluster_mu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic(Cluster *)  g_cluster_pages[CLUSTER_PAGES];
static _Atomic u32         g_cluster_count;
static _Atomic u64         g_cluster_refused;
static _Atomic usize       g_cluster_bytes;
static u32                *g_cluster_hash;      /* open addressing: 0 empty, else idx+1; mu */
static u32                 g_cluster_hash_cap;  /* mu */
static atomic_flag         g_cluster_logged = ATOMIC_FLAG_INIT;

static u32 cluster_hash(const u32 *cps, u32 n) {
    u32 h = 0x811C9DC5u;
    for (u32 i = 0; i < n; i++) {
        h ^= cps[i];
        h *= 0x01000193u;
    }
    return h;
}

static const Cluster *cluster_at(u32 idx) {
    Cluster *pg = atomic_load_explicit(&g_cluster_pages[idx >> CLUSTER_PAGE_SHIFT],
                                       memory_order_acquire);
    return pg ? &pg[idx & (CLUSTER_PAGE_SIZE - 1u)] : NULL;
}

static void cluster_refuse(const char *why) {
    atomic_fetch_add_explicit(&g_cluster_refused, 1, memory_order_relaxed);
    if (!atomic_flag_test_and_set(&g_cluster_logged))
        fprintf(stderr, "utf8: grapheme cluster table %s (%u entries); "
                "further clusters draw as their first codepoint\n",
                why, atomic_load_explicit(&g_cluster_count, memory_order_relaxed));
}

/* Double the hash. Entries are re-derived from the pages, so a handle never
 * changes. On failure the old table stays and the caller decides. */
static bool cluster_hash_grow(void) {
    u32 ncap = g_cluster_hash_cap ? g_cluster_hash_cap * 2u : CLUSTER_HASH_START;
    u32 *next = (u32 *)calloc(ncap, sizeof(u32));
    if (!next) return false;
    u32 mask = ncap - 1u;
    u32 count = atomic_load_explicit(&g_cluster_count, memory_order_relaxed);
    for (u32 i = 0; i < count; i++) {
        const Cluster *c = cluster_at(i);
        u32 slot = cluster_hash(c->cps, c->n) & mask;
        while (next[slot] != 0) slot = (slot + 1u) & mask;
        next[slot] = i + 1u;
    }
    free(g_cluster_hash);
    atomic_fetch_add_explicit(&g_cluster_bytes,
        (usize)ncap * sizeof(u32) - (usize)g_cluster_hash_cap * sizeof(u32),
        memory_order_relaxed);
    g_cluster_hash = next;
    g_cluster_hash_cap = ncap;
    return true;
}

static u32 cluster_intern_locked(const u32 *cps, u32 n) {
    if (!g_cluster_hash && !cluster_hash_grow()) { cluster_refuse("out of memory"); return cps[0]; }

    u32 mask = g_cluster_hash_cap - 1u;
    u32 slot = cluster_hash(cps, n) & mask;
    for (u32 probe = 0; probe < g_cluster_hash_cap; probe++) {
        u32 entry = g_cluster_hash[slot];
        if (entry == 0) break;            /* miss — insert below */
        const Cluster *c = cluster_at(entry - 1u);
        if (c->n == n && memcmp(c->cps, cps, n * sizeof(u32)) == 0)
            return UTF8_CLUSTER_BIT | (entry - 1u);
        slot = (slot + 1u) & mask;
    }

    /* Table full: degrade to the base codepoint rather than evicting a handle
     * that scrollback still points at. Counted, and said once. */
    u32 idx = atomic_load_explicit(&g_cluster_count, memory_order_relaxed);
    if (idx >= CLUSTER_MAX_COUNT) { cluster_refuse("is full"); return cps[0]; }

    /* Keep the hash at most half full; the load factor is what keeps the
     * probe chains short. A failed grow is tolerable until it is nearly full. */
    if ((idx + 1u) * 2u > g_cluster_hash_cap) {
        if (!cluster_hash_grow() && (idx + 1u) * 4u > g_cluster_hash_cap * 3u) {
            cluster_refuse("hash out of memory");
            return cps[0];
        }
        mask = g_cluster_hash_cap - 1u;
        slot = cluster_hash(cps, n) & mask;
        while (g_cluster_hash[slot] != 0) slot = (slot + 1u) & mask;
    }

    u32 page = idx >> CLUSTER_PAGE_SHIFT;
    Cluster *pg = atomic_load_explicit(&g_cluster_pages[page], memory_order_relaxed);
    if (!pg) {
        pg = (Cluster *)calloc(CLUSTER_PAGE_SIZE, sizeof(Cluster));
        if (!pg) { cluster_refuse("out of memory"); return cps[0]; }
        atomic_fetch_add_explicit(&g_cluster_bytes, CLUSTER_PAGE_SIZE * sizeof(Cluster),
                                  memory_order_relaxed);
        atomic_store_explicit(&g_cluster_pages[page], pg, memory_order_release);
    }
    Cluster *c = &pg[idx & (CLUSTER_PAGE_SIZE - 1u)];
    memcpy(c->cps, cps, n * sizeof(u32));
    c->n = n;
    g_cluster_hash[slot] = idx + 1u;
    /* Publish AFTER the entry is whole: a lock-free reader that sees the new
     * count sees the entry. */
    atomic_store_explicit(&g_cluster_count, idx + 1u, memory_order_release);
    return UTF8_CLUSTER_BIT | idx;
}

u32 utf8_cluster_intern(const u32 *cps, u32 n) {
    if (!cps || n == 0) return 0;
    if (n == 1) return cps[0];
    if (n > UTF8_CLUSTER_MAX) n = UTF8_CLUSTER_MAX;
    pthread_mutex_lock(&g_cluster_mu);
    u32 r = cluster_intern_locked(cps, n);
    pthread_mutex_unlock(&g_cluster_mu);
    return r;
}

const u32 *utf8_cluster_lookup(u32 handle, u32 *out_n) {
    if (!utf8_is_cluster(handle)) return NULL;
    u32 idx = handle & ~UTF8_CLUSTER_BIT;
    if (idx >= atomic_load_explicit(&g_cluster_count, memory_order_acquire)) return NULL;
    const Cluster *c = cluster_at(idx);
    if (!c) return NULL;
    if (out_n) *out_n = c->n;
    return c->cps;
}

u32   utf8_cluster_count(void)    { return atomic_load_explicit(&g_cluster_count, memory_order_acquire); }
u32   utf8_cluster_capacity(void) { return CLUSTER_MAX_COUNT; }
u64   utf8_cluster_refused(void)  { return atomic_load_explicit(&g_cluster_refused, memory_order_relaxed); }
usize utf8_cluster_bytes(void)    { return atomic_load_explicit(&g_cluster_bytes, memory_order_relaxed); }

u32 utf8_cluster_base(u32 v) {
    u32 n = 0;
    const u32 *cps = utf8_cluster_lookup(v, &n);
    if (!cps || n == 0) return utf8_is_cluster(v) ? ' ' : v;
    return cps[0];
}

u32 utf8_cluster_encode(u32 v, u8 *out, u32 cap) {
    if (!out) return 0;
    u32 n = 0;
    const u32 *cps = utf8_cluster_lookup(v, &n);
    if (!cps) {
        if (utf8_is_cluster(v)) return 0;     /* stale handle, no table */
        if (cap < 4) {
            u8 tmp[4];
            u32 len = utf8_encode(v, tmp);
            if (len == 0 || len > cap) return 0;
            memcpy(out, tmp, len);
            return len;
        }
        return utf8_encode(v, out);
    }
    u32 written = 0;
    for (u32 i = 0; i < n; i++) {
        u8 tmp[4];
        u32 len = utf8_encode(cps[i], tmp);
        if (len == 0) continue;
        if (written + len > cap) break;
        memcpy(out + written, tmp, len);
        written += len;
    }
    return written;
}

u32 utf8_next_grapheme(const u8 *s, usize len, u32 *out_cp, i32 *out_cols) {
    if (!s || len == 0) return 0;
    u32 base = 0;
    u32 used = utf8_decode(s, len, &base);
    if (used == 0) return 0;

    u32 cps[UTF8_CLUSTER_MAX];
    u32 n = 1;
    cps[0] = base;
    usize consumed = used;

    while (n < UTF8_CLUSTER_MAX && consumed < len) {
        u32 next = 0;
        u32 nn = utf8_decode(s + consumed, len - consumed, &next);
        if (nn == 0) break;
        /* Joiners always attach; a codepoint after ZWJ continues the sequence;
         * two regional indicators pair into one flag (and only two). */
        bool join = utf8_is_joiner(next) || cps[n - 1] == 0x200D ||
                    (n == 1 && next >= 0x1F1E6 && next <= 0x1F1FF &&
                     base >= 0x1F1E6 && base <= 0x1F1FF);
        if (!join) break;
        cps[n++] = next;
        consumed += nn;
    }

    u32 cp = (n > 1) ? utf8_cluster_intern(cps, n) : base;
    if (out_cp) *out_cp = cp;
    if (out_cols) *out_cols = utf8_char_width(cp);
    return (u32)consumed;
}

usize utf8_display_cols(const u8 *s, usize byte_len) {
    if (!s) return 0;
    usize cols = 0, i = 0;
    while (i < byte_len) {
        u32 cp = 0;
        i32 w = 0;
        u32 used = utf8_next_grapheme(s + i, byte_len - i, &cp, &w);
        if (used == 0) { i++; continue; }
        if (w > 0) cols += (usize)w;
        i += used;
    }
    return cols;
}

/* Codepoints that join onto the PRECEDING grapheme instead of opening a cell.
 * Everything here has width 0 by definition — it is part of the cluster the
 * base character started.
 *
 * The list is generated from the Unicode data, not typed by hand. A mark that
 * opened a cell of its own would cost more than a column: the shaper never
 * sees base+mark together, because they never become one cluster, so Indic and
 * Arabic lose their shaping entirely; and the width the terminal draws stops
 * matching the width the program on the other end of the pty computed, which
 * is the contract MODE_GRAPHEME exists to keep. U+3099/U+309A sit inside the
 * CJK width ranges below, so they must be caught here or NFD Japanese (what
 * macOS hands back for every filename) draws at double width with the dakuten
 * in a cell of its own.
 *
 * src/core/utf8_marks.h is General_Category Mn + Me straight out of the
 * Unicode data; scripts/gen_utf8_marks.py regenerates it and --check verifies
 * it. The three joiners that are NOT marks stay here, next to their reasons.
 *
 * Mc (spacing combining marks) are deliberately absent: they advance the pen
 * in every wcwidth implementation, so folding them into the preceding cell
 * would break the same contract from the other side. */
bool utf8_is_joiner(u32 cp) {
    /* Nothing below U+0300 joins — verified against the generated table, whose
     * first range starts exactly there — and a cluster handle carries bit 30,
     * so this cannot swallow one. This is the ladder's only early-out and it
     * covers all of ASCII, Latin-1 and IPA. */
    if (cp < 0x0300) return false;
    /* Combining Diacritical Marks is the first range of the table and by far
     * the most common thing that reaches here; answering it before anything
     * else keeps Latin-with-accents off both lookups below. */
    if (cp <= 0x036F) return true;

    /* The block bitmap decides the common case in three instructions, and it has
     * to come before the binary search rather than after it. utf8_char_width
     * asks this question BEFORE its CJK width ladder — it has to, because
     * U+3099/U+309A are marks sitting inside the CJK ranges — so every ideograph
     * and every emoji pays whatever this costs. One bit per 64 codepoints clears
     * 99% of the space, and a clear bit is a definite no. A cluster handle
     * carries bit 30, so it lands past the map and the bound test answers for
     * it. */
    u32 blk = cp >> UTF8_MARK_BLOCK_SHIFT;
    if (blk >= UTF8_MARK_BLOCK_BITS) return false;
    if (!((g_utf8_mark_blocks[blk >> 3] >> (blk & 7)) & 1u)) return false;

    /* The joiners that are NOT Mn/Me, so the table below cannot hold them.
     * Their blocks are set in the bitmap for exactly this reason. ZWJ and the
     * tag characters are Cf, the skin tones Sk, and the conjoining Hangul jamo
     * are Lo — a medial vowel (V) and a final consonant (T) compose onto the
     * initial (L) that opened the cell, which is the rule every wcwidth
     * implementation carries; without it an L+V pair would measure three
     * columns. U+D7B0.. are the Extended-A medials and Extended-B finals. */
    if (cp == 0x200D) return true;                      /* ZWJ */
    if (cp >= 0x1F3FB && cp <= 0x1F3FF) return true;    /* Skin tone modifiers */
    if (cp >= 0xE0020 && cp <= 0xE007F) return true;    /* Tag characters */
    if (cp >= 0x1160 && cp <= 0x11FF) return true;      /* Hangul jamo V + T */
    if (cp >= 0xD7B0 && cp <= 0xD7C6) return true;
    if (cp >= 0xD7CB && cp <= 0xD7FB) return true;

    /* Binary search — the table is sorted and non-overlapping by construction
     * (the generator emits it that way), so ~8 compares decide any codepoint
     * whose block survived the map. */
    u32 lo = 0, hi = (u32)(sizeof(g_utf8_mark_ranges) / sizeof(g_utf8_mark_ranges[0]));
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2u;
        if (cp < g_utf8_mark_ranges[mid].lo)      hi = mid;
        else if (cp > g_utf8_mark_ranges[mid].hi) lo = mid + 1u;
        else                                      return true;
    }
    return false;
}

/* ---- The width table --------------------------------------------------------
 * Two bits a codepoint over planes 0 and 1 — every script and every emoji:
 * 0, 1 or 2 cells, or 3 = an emoji (two cells, drawn in colour). A text-default
 * emoji is class 1 (one cell) and utf8_is_emoji asks its own table for it. It is the
 * ladders below, remembered: a block of 256 codepoints is filled from them the
 * first time anything in it is asked about, so the table holds exactly what
 * the ladders say and only the blocks a session prints.
 *
 * Why: put_char asks the width of every character it prints, and past Latin
 * the ladder is a walk — the joiner bitmap, eleven format-character compares,
 * twelve CJK ranges, then for an emoji or a symbol forty more in
 * utf8_is_emoji.
 *
 * Any thread may ask (text is measured on workers). A block goes 0 -> 1 by
 * compare-and-swap, is filled by the thread that won, and is published 1 -> 2
 * with release; a thread that finds it at 1 answers from the ladder instead of
 * waiting. */
#define UTF8_WTAB_LIMIT       0x20000u
#define UTF8_WTAB_BLOCK_SHIFT 8u
#define UTF8_WTAB_BLOCKS      (UTF8_WTAB_LIMIT >> UTF8_WTAB_BLOCK_SHIFT)
#define UTF8_WTAB_EMOJI       3u
static u8         g_wtab[UTF8_WTAB_LIMIT / 4u];
static _Atomic u8 g_wtab_state[UTF8_WTAB_BLOCKS];   /* 0 empty, 1 filling, 2 ready */

static bool utf8_wtab_fill(u32 block) {
    u8 expected = 0;
    if (!atomic_compare_exchange_strong_explicit(&g_wtab_state[block], &expected, 1,
                                                 memory_order_acq_rel, memory_order_acquire))
        return expected == 2;
    u32 base = block << UTF8_WTAB_BLOCK_SHIFT;
    for (u32 i = 0; i < (1u << UTF8_WTAB_BLOCK_SHIFT); i += 4u) {
        u8 packed = 0;
        for (u32 k = 0; k < 4u; k++) {
            u32 cp = base + i + k;
            u32 w = (u32)utf8_char_width_ladder(cp);
            u32 c = (w == 2u && utf8_is_emoji_ladder(cp)) ? UTF8_WTAB_EMOJI : w;
            packed |= (u8)(c << (k * 2u));
        }
        g_wtab[(base + i) >> 2] = packed;
    }
    atomic_store_explicit(&g_wtab_state[block], 2, memory_order_release);
    return true;
}

/* The class of `cp` (< UTF8_WTAB_LIMIT), or -1 while another thread fills its
 * block. */
static inline i32 utf8_wtab_class(u32 cp) {
    u32 block = cp >> UTF8_WTAB_BLOCK_SHIFT;
    if (atomic_load_explicit(&g_wtab_state[block], memory_order_acquire) != 2 &&
        !utf8_wtab_fill(block)) return -1;
    return (i32)((g_wtab[cp >> 2] >> ((cp & 3u) << 1)) & 3u);
}

int utf8_char_width(u32 cp) {
    /* ASCII fast path. Printable ASCII is one cell and cannot be a cluster, a
     * joiner, an emoji or a fullwidth form — every range tested below starts
     * above U+007F. put_char asks this for every character that arrives from
     * the PTY, so the full ladder (~70 compares, most of them inside
     * utf8_is_emoji) ran per byte of ordinary terminal output. */
    if (cp < 0x80) return (cp >= 0x20 && cp < 0x7F) ? 1 : 0;
    /* U+00A0..U+02FF are all one cell (see the ladder), and a table load is
     * still more than a compare. */
    if (cp >= 0xA0 && cp < 0x0300) return 1;
    if (cp < UTF8_WTAB_LIMIT) {
        i32 c = utf8_wtab_class(cp);
        if (c >= 0) return c == (i32)UTF8_WTAB_EMOJI ? 2 : c;
    }
    return utf8_char_width_ladder(cp);
}

int utf8_char_width_ladder(u32 cp) {
    if (cp < 0x80) return (cp >= 0x20 && cp < 0x7F) ? 1 : 0;
    if (utf8_is_cluster(cp)) {
        /* A cluster is as wide as what it renders to: the emoji path draws a
         * two-cell quad, a letter+diacritic stays one. */
        return utf8_is_emoji(cp) ? 2 : utf8_char_width(utf8_cluster_base(cp));
    }
    /* U+00A0..U+02FF are all one cell under the width policy this function
     * implements. They precede every combining/joiner and emoji range, so the
     * common Latin/IPA path can return without walking either range ladder. */
    if (cp >= 0xA0 && cp < 0x0300) return 1;
    if (utf8_is_joiner(cp)) return 0;
    /* Format characters that draw nothing: zero-width space and non-joiner,
     * the directional marks, embeddings and isolates, word joiner and the
     * invisible operators, a file's byte-order mark. Each would otherwise take
     * a cell: a BOM would shift the first line of a `cat`, a ZWNJ would split
     * a Persian word. */
    if ((cp >= 0x200B && cp <= 0x200F) || (cp >= 0x202A && cp <= 0x202E) ||
        (cp >= 0x2060 && cp <= 0x2064) || (cp >= 0x2066 && cp <= 0x206F) ||
        cp == 0xFEFF || cp == 0x061C || (cp >= 0xFFF9 && cp <= 0xFFFB))
        return 0;
    /* C0/C1 control characters */
    if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) return 0;
    /* Two columns for East Asian Width Wide and Fullwidth — CJK, Hangul, kana,
     * the fullwidth forms and every emoji-presentation emoji — and for the
     * unassigned rest of planes 2 and 3, which the standard reserves for
     * ideographs. A text-default emoji (U+1F5A5 without VS16) is one column,
     * as a program's wcwidth counts it; with VS16 it is a cluster, measured
     * above. */
    if (cp >= 0x20000 && cp <= 0x3FFFD) return 2;
    if (cp >= 0x1100 &&
        utf8_range_find(g_utf8_wide_ranges, UTF8_RANGE_COUNT(g_utf8_wide_ranges), cp))
        return 2;
    return 1;
}

bool utf8_is_emoji(u32 cp) {
    /* U+231A (⌚) is the lowest codepoint the ladder can accept, and a cluster
     * handle is >= UTF8_CLUSTER_BIT, so this early-out is exact. It exists
     * because the terminal asks this question for EVERY character it prints. */
    if (cp < 0x231A) return false;
    if (cp < UTF8_WTAB_LIMIT) {
        i32 c = utf8_wtab_class(cp);
        if (c == (i32)UTF8_WTAB_EMOJI) return true;
        /* One column and still an emoji: a text-default emoji or a lone
         * regional indicator. */
        if (c >= 0) return c == 1 && cp >= 0x1F000 && utf8_is_emoji_ladder(cp);
    }
    return utf8_is_emoji_ladder(cp);
}

bool utf8_is_emoji_ladder(u32 cp) {
    /* U+231A (⌚) is the lowest codepoint any branch below can accept, and a
     * cluster handle is >= UTF8_CLUSTER_BIT, so this early-out is exact. It
     * exists because the terminal asks this question for EVERY character it
     * prints. */
    if (cp < 0x231A) return false;
    if (utf8_is_cluster(cp)) {
        /* A cluster renders in colour when anything in it forces emoji
         * presentation: an emoji base, VS16, a keycap mark, or a flag pair.
         * `❤️` (U+2764 U+FE0F) qualifies while a bare U+2764 does not. VS15
         * asks for the text presentation, so a base followed by it alone
         * stays text. */
        u32 n = 0;
        const u32 *cps = utf8_cluster_lookup(cp, &n);
        if (!cps) return false;
        bool emoji = false, text = false;
        for (u32 i = 0; i < n; i++) {
            if (cps[i] == 0xFE0F || cps[i] == 0x20E3) return true;
            if (cps[i] == 0xFE0E) text = true;
            else if (utf8_is_emoji(cps[i])) emoji = true;
        }
        return emoji && !text;
    }
    if (utf8_is_joiner(cp)) return false;              /* VS/ZWJ/skin tone — modifiers, not emoji */
    /* Emoji_Presentation, and from U+1F000 the emoji whose default is text:
     * those take one column (utf8_char_width) but no face on the system draws
     * them as text, so they are drawn in colour too. Below U+1F000 a
     * text-default emoji (U+2764, U+263A) is drawn from a text face. */
    if (utf8_range_find(g_utf8_emoji_pres_ranges, UTF8_RANGE_COUNT(g_utf8_emoji_pres_ranges), cp))
        return true;
    return cp >= 0x1F000 &&
           utf8_range_find(g_utf8_emoji_text_ranges, UTF8_RANGE_COUNT(g_utf8_emoji_text_ranges), cp);
}
