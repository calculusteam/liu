/* Native equivalents of the model's ordered Split/ByteLevel pretokenizers.
 * Hunyuan reference: tencent/HY-MT1.5-1.8B tokenizer.json, revision
 * 74fc0f7236a2400d6bbda892ed7ff0e448e000cc. Split boundaries are hard BPE
 * boundaries; unmatched spans are preserved, as in tokenizers::Split. */
#include "llm/llm_pretokenizer.h"
#include "llm/llm_unicode_categories.h"
#include "core/utf8.h"
#include <string.h>

static const u8 *scalar(const u8 *p, const u8 *end, u32 *cp) {
    u32 n = utf8_decode(p, (usize)(end - p), cp);
    if (!n) { *cp = 0xFFFD; n = 1; }
    return p + n;
}

static const u8 *run(const u8 *p, const u8 *end, u8 mask) {
    while (p < end) {
        u32 cp;
        const u8 *next = scalar(p, end, &cp);
        if (!(llm_unicode_flags(cp) & mask)) break;
        p = next;
    }
    return p;
}

static bool cjk(u32 cp) {
    return (cp >= 0x4E00 && cp <= 0x9FA5) ||
           (cp >= 0x3040 && cp <= 0x30FF);
}

static bool ascii_letter(u32 cp) {
    return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z');
}

/* Common ordered whitespace alternatives: \s*[\r\n]+, \s+(?!\S), \s+.
 * The first is present only in Hunyuan. Greedy backtracking leaves the last
 * whitespace for a following word when the negative lookahead requires it. */
static const u8 *whitespace(const u8 *p, const u8 *end, bool hunyuan) {
    const u8 *q = p, *last_start = p, *newline_end = NULL;
    while (q < end) {
        u32 cp;
        const u8 *next = scalar(q, end, &cp);
        if (!(llm_unicode_flags(cp) & LLM_U_SPACE)) break;
        last_start = q;
        q = next;
        if (cp == '\r' || cp == '\n') newline_end = next;
    }
    if (hunyuan && newline_end) return newline_end;
    if (q == end) return q;
    return last_start > p ? last_start : q;
}

static const u8 *hunyuan_match(const u8 *p, const u8 *end) {
    u32 cp;
    const u8 *next = scalar(p, end, &cp);
    u8 f = llm_unicode_flags(cp);
    /* ASCII punctuation followed by ASCII letters (including 'm / 't). */
    if (cp < 128 && (f & (LLM_U_P | LLM_U_S))) {
        const u8 *q = next;
        while (q < end && ascii_letter(*q)) q++;
        if (q > next) return q;
    }
    /* [^\r\n\p{L}\p{P}\p{S}]?[\p{L}\p{M}]+ */
    if (cp != '\r' && cp != '\n' && !(f & (LLM_U_L | LLM_U_P | LLM_U_S))) {
        const u8 *q = run(next, end, LLM_U_L | LLM_U_M);
        if (q > next) return q;
    }
    if (f & (LLM_U_L | LLM_U_M)) return run(p, end, LLM_U_L | LLM_U_M);
    /* An optional ASCII space, punctuation/symbols, then CR/LF. */
    const u8 *q = run(cp == ' ' ? next : p, end, LLM_U_P | LLM_U_S);
    if (q > (cp == ' ' ? next : p)) {
        while (q < end && (*q == '\r' || *q == '\n')) q++;
        return q;
    }
    if (f & LLM_U_SPACE) return whitespace(p, end, true);
    return NULL;
}

static const u8 *gpt2_match(const u8 *p, const u8 *end) {
    if (*p == '\'') {
        static const char *const contractions[] = {"'s", "'t", "'re", "'ve", "'m", "'ll", "'d"};
        for (usize i = 0; i < sizeof contractions / sizeof *contractions; i++) {
            usize n = strlen(contractions[i]);
            if ((usize)(end - p) >= n && memcmp(p, contractions[i], n) == 0) return p + n;
        }
    }
    const u8 *start = *p == ' ' && p + 1 < end ? p + 1 : p;
    u32 cp;
    (void)scalar(start, end, &cp);
    u8 f = llm_unicode_flags(cp);
    if (f & LLM_U_L) return run(start, end, LLM_U_L);
    if (f & LLM_U_N) return run(start, end, LLM_U_N);
    if (!(f & LLM_U_SPACE)) {
        const u8 *q = start;
        while (q < end) {
            const u8 *next = scalar(q, end, &cp);
            if (llm_unicode_flags(cp) & (LLM_U_SPACE | LLM_U_L | LLM_U_N)) break;
            q = next;
        }
        return q;
    }
    return whitespace(p, end, false);
}

void llm_pre_init(LlmPreTokenizer *it, const char *text, LlmPreType type) {
    it->p = (const u8 *)text;
    it->end = it->p + strlen(text);
    it->number_end = it->segment_end = it->p;
    it->type = type;
}

bool llm_pre_next(LlmPreTokenizer *it, const u8 **start, usize *len) {
    const u8 *p = it->p;
    if (p >= it->end) return false;
    const u8 *end = it->end;
    if (it->type == LLM_PRE_HUNYUAN) {
        /* Split 1: isolate numbers in groups of at most three scalars. */
        if (p >= it->number_end) {
            u32 cp;
            (void)scalar(p, end, &cp);
            bool number = (llm_unicode_flags(cp) & LLM_U_N) != 0;
            const u8 *q = p;
            i32 n = 0;
            while (q < end) {
                const u8 *next = scalar(q, end, &cp);
                if (((llm_unicode_flags(cp) & LLM_U_N) != 0) != number || (number && n == 3)) break;
                q = next;
                if (number) n++;
            }
            it->number_end = q;
        }
        /* Split 2: isolate the reference's CJK/kana ranges. */
        if (p >= it->segment_end) {
            u32 cp;
            (void)scalar(p, it->number_end, &cp);
            bool asian = cjk(cp);
            const u8 *q = p;
            while (q < it->number_end) {
                const u8 *next = scalar(q, it->number_end, &cp);
                if (cjk(cp) != asian) break;
                q = next;
            }
            it->segment_end = q;
        }
        end = it->segment_end;
    }
    const u8 *q = it->type == LLM_PRE_HUNYUAN ? hunyuan_match(p, end) : gpt2_match(p, end);
    if (!q) {
        /* Isolated Split preserves text which does not match any alternative. */
        q = p;
        do { u32 cp; q = scalar(q, end, &cp); }
        while (q < end && !hunyuan_match(q, end));
    }
    *start = p; *len = (usize)(q - p); it->p = q;
    return true;
}
