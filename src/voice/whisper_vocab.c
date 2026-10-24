/*
 * Liu - Whisper vocabulary + special-token resolution.
 *
 * The offsets below mirror whisper.cpp's whisper_vocab exactly; see the
 * header for why they are computed rather than hardcoded.
 */
#include "voice/whisper_vocab.h"

#include <string.h>

/* Base ids, valid for the English-only models. Multilingual models shift
 * eot/sot by one and everything from token_translate up by
 * (n_languages - 98). */
#define WV_BASE_EOT        50256
#define WV_BASE_SOT        50257
#define WV_BASE_TRANSLATE  50357
#define WV_BASE_TRANSCRIBE 50358
#define WV_BASE_SOLM       50359
#define WV_BASE_PREV       50360
#define WV_BASE_NOSP       50361
#define WV_BASE_NOT        50362
#define WV_BASE_BEG        50363

/* Whisper's language table in id order — index 0 is <|en|>, and a language
 * token is sot + 1 + index. Order is load-bearing: it is the model's, not
 * alphabetical, and it must match whisper.cpp's g_lang insertion order. */
static const char *const g_lang_codes[] = {
    "en","zh","de","es","ru","ko","fr","ja","pt","tr",
    "pl","ca","nl","ar","sv","it","id","hi","fi","vi",
    "he","uk","el","ms","cs","ro","da","hu","ta","no",
    "th","ur","hr","bg","lt","la","mi","ml","cy","sk",
    "te","fa","lv","bn","sr","az","sl","kn","et","mk",
    "br","eu","is","hy","ne","mn","bs","kk","sq","sw",
    "gl","mr","pa","si","km","sn","yo","so","af","oc",
    "ka","be","tg","sd","gu","am","yi","lo","uz","fo",
    "ht","ps","tk","nn","mt","sa","lb","my","bo","tl",
    "mg","as","tt","haw","ln","ha","ba","jw","su","yue",
};

#define WV_LANG_COUNT ((i32)(sizeof g_lang_codes / sizeof *g_lang_codes))

i32 whisper_vocab_lang_count(void) { return WV_LANG_COUNT; }

const char *whisper_vocab_lang_code(i32 index) {
    if (index < 0 || index >= WV_LANG_COUNT) return NULL;
    return g_lang_codes[index];
}

bool whisper_vocab_init(WhisperVocab *v, const WhisperFile *w) {
    if (!v || !w) return false;
    memset(v, 0, sizeof *v);

    v->w       = w;
    v->n_vocab = w->h.n_vocab;

    /* whisper.cpp's own predicate. 51865 is v2's multilingual size; v3 and
     * turbo declare 51866 because they add the Cantonese token. */
    v->multilingual = (w->h.n_vocab >= 51865);
    v->n_languages  = w->h.n_vocab - 51765 - (v->multilingual ? 1 : 0);

    v->token_eot        = WV_BASE_EOT;
    v->token_sot        = WV_BASE_SOT;
    v->token_translate  = WV_BASE_TRANSLATE;
    v->token_transcribe = WV_BASE_TRANSCRIBE;
    v->token_solm       = WV_BASE_SOLM;
    v->token_prev       = WV_BASE_PREV;
    v->token_nosp       = WV_BASE_NOSP;
    v->token_not        = WV_BASE_NOT;
    v->token_beg        = WV_BASE_BEG;

    if (v->multilingual) {
        if (v->n_languages < 1 || v->n_languages > WV_LANG_COUNT) return false;
        v->token_eot++;
        v->token_sot++;
        const i32 dt = v->n_languages - 98;
        v->token_translate  += dt;
        v->token_transcribe += dt;
        v->token_solm       += dt;
        v->token_prev       += dt;
        v->token_nosp       += dt;
        v->token_not        += dt;
        v->token_beg        += dt;
    } else {
        v->n_languages = 0;
    }

    /* Every synthesized id must land inside the declared vocabulary, or the
     * decoder would sample a logit that does not exist. */
    if (v->token_beg >= v->n_vocab) return false;
    return true;
}

i32 whisper_vocab_lang_token(const WhisperVocab *v, const char *code) {
    if (!v || !code || !v->multilingual) return -1;
    for (i32 i = 0; i < v->n_languages && i < WV_LANG_COUNT; i++) {
        if (strcmp(g_lang_codes[i], code) == 0) return v->token_sot + 1 + i;
    }
    return -1;
}

i32 whisper_vocab_token_lang(const WhisperVocab *v, i32 token) {
    if (!v || !v->multilingual) return -1;
    i32 idx = token - v->token_sot - 1;
    if (idx < 0 || idx >= v->n_languages) return -1;
    return idx;
}

bool whisper_vocab_append(const WhisperVocab *v, i32 token,
                          char *out, i32 *len, i32 cap) {
    if (!v || !out || !len) return false;
    if (token < 0 || token >= v->n_vocab) return true;      /* nothing to add */
    if (token >= v->token_eot) return true;                 /* special/timestamp */
    if (token >= v->w->n_vocab_file) return true;           /* synthesized gap */

    const WhisperTokenStr *t = &v->w->vocab[token];
    if (t->len <= 0) return true;
    if (*len > cap - t->len) return false;

    memcpy(out + *len, t->p, (usize)t->len);
    *len += t->len;
    return true;
}
