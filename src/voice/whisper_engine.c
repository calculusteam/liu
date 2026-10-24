/*
 * Whisper speech-to-text engine using greedy decoding at temperature 0 and
 * Whisper's logit constraints. The constraints prevent invalid token and
 * timestamp sequences:
 *
 * - Control and task tokens are not sampled.
 * - The first sampled token cannot be EOT or a bare space.
 * - Timestamps occur in pairs around text and never decrease.
 * - The first timestamp is capped at 1.0 seconds into the window.
 * - A timestamp is forced when its total probability exceeds that of the best
 *   text token.
 *
 * Seeking uses the last emitted timestamp, so the next window begins at the
 * end of the last complete segment rather than a fixed 30-second boundary.
 */
#include "voice/whisper_engine.h"
#include "voice/whisper_cpu.h"
#include "voice/whisper_ggml.h"
#include "voice/whisper_mel.h"
#include "voice/whisper_model.h"
#include "voice/whisper_vocab.h"
#include "voice/voice_paths.h"   /* LIU_WHISPER_TRANSLATE_MIN_DECODER_LAYERS */

#ifdef LIU_WHISPER_METAL
#include "voice/whisper_metal.h"
#endif
#ifdef LIU_WHISPER_VULKAN
#include "voice/whisper_vulkan.h"
#endif

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Whisper's own default: the first timestamp of a window may not be later
 * than 1 s in. Stops the decoder from skipping over a quiet lead-in. */
#define WE_MAX_INITIAL_TS_SEC 1.0f

/* A window that produces no timestamp at all still has to advance, or the
 * outer loop spins on the same audio forever. */
#define WE_FULL_WINDOW WHISPER_N_FRAMES

/* Stop when less than 100 ms of audio remains (10 mel frames) to avoid
 * passing a silence-only window to the encoder and decoder, which can
 * produce filler tokens. */
#define WE_DELTA_MIN 10

struct WhisperEngine {
    WhisperFile     file;
    WhisperWeights  weights;
    WhisperVocab    vocab;

    WhisperCpu     *cpu;
#ifdef LIU_WHISPER_METAL
    WhisperMetal   *mtl;
#endif
#ifdef LIU_WHISPER_VULKAN
    WhisperVulkan  *vk;
#endif
    const char     *backend;

    f32  *mel_win;      /* [n_mels * WHISPER_N_FRAMES] */
    f32  *logits;       /* [n_vocab] */
    i32  *tokens;       /* [n_text_ctx] prompt + generated */

    char  language[8];

    WhisperProgressFn progress;
    void             *progress_user;
    f32               progress_fraction;
    bool              cancelled;
};

WhisperParams whisper_params_default(void) {
    WhisperParams p;
    memset(&p, 0, sizeof p);
    p.translate  = false;
    p.max_tokens = 0;
    return p;
}

const char *whisper_engine_backend(const WhisperEngine *e) {
    return e && e->backend ? e->backend : "";
}

bool whisper_engine_can_translate(const WhisperEngine *e) {
    /* Decoder depth, exactly as liu_whisper_can_translate() reads it off the
     * header — one rule, asked here of the loaded model and there of a file
     * Settings has not opened. */
    if (!e || !e->vocab.multilingual) return false;
    return e->file.h.n_text_layer >= LIU_WHISPER_TRANSLATE_MIN_DECODER_LAYERS;
}

const char *whisper_engine_language(const WhisperEngine *e) {
    return e ? e->language : "";
}

void whisper_engine_set_progress(WhisperEngine *e, WhisperProgressFn fn, void *user) {
    if (!e) return;
    e->progress = fn;
    e->progress_user = user;
}

static bool we_cancelled(void *user) {
    WhisperEngine *e = user;
    if (!e->cancelled && e->progress &&
        !e->progress(e->progress_user, e->progress_fraction)) e->cancelled = true;
    return e->cancelled;
}

void whisper_engine_free(WhisperEngine *e) {
    if (!e) return;
#ifdef LIU_WHISPER_METAL
    whisper_metal_free(e->mtl);
#endif
#ifdef LIU_WHISPER_VULKAN
    whisper_vulkan_free(e->vk);
#endif
    whisper_cpu_free(e->cpu);
    whisper_weights_free(&e->weights);
    whisper_close(&e->file);
    free(e->mel_win);
    free(e->logits);
    free(e->tokens);
    free(e);
}

WhisperEngine *whisper_engine_load(const char *path, char *err, usize err_cap) {
    if (err && err_cap) err[0] = '\0';

    WhisperEngine *e = calloc(1, sizeof *e);
    if (!e) { if (err && err_cap) snprintf(err, err_cap, "out of memory"); return NULL; }

    if (!whisper_open(&e->file, path, err, err_cap)) { free(e); return NULL; }
    if (!whisper_weights_load(&e->weights, &e->file, err, err_cap)) {
        whisper_close(&e->file); free(e); return NULL;
    }
    if (!whisper_vocab_init(&e->vocab, &e->file)) {
        if (err && err_cap) snprintf(err, err_cap, "inconsistent vocabulary metadata");
        whisper_weights_free(&e->weights); whisper_close(&e->file); free(e);
        return NULL;
    }
    /* The mel front end is fixed at Whisper's 400-point transform, so a model
     * whose filterbank was built for a different one cannot be run. */
    if (e->file.n_fft != 1 + WHISPER_N_FFT / 2 || e->file.n_mel != e->file.h.n_mels) {
        if (err && err_cap) snprintf(err, err_cap, "model mel filterbank has an unexpected shape");
        whisper_weights_free(&e->weights); whisper_close(&e->file); free(e);
        return NULL;
    }

    e->mel_win = malloc((usize)e->file.h.n_mels * WHISPER_N_FRAMES * sizeof(f32));
    e->logits  = malloc((usize)e->file.h.n_vocab * sizeof(f32));
    e->tokens  = malloc((usize)e->file.h.n_text_ctx * sizeof(i32));
    if (!e->mel_win || !e->logits || !e->tokens) {
        if (err && err_cap) snprintf(err, err_cap, "out of memory");
        whisper_engine_free(e);
        return NULL;
    }

#ifdef LIU_WHISPER_METAL
    /* When LIU_WHISPER_FORCE_CPU is set, skip GPU backend selection and use
     * the portable C implementation. */
    {
        const char *force = getenv("LIU_WHISPER_FORCE_CPU");
        if (!(force && force[0] && force[0] != '0')) {
            e->mtl = whisper_metal_create(&e->weights, NULL, 0);
            if (e->mtl) e->backend = "metal";
        }
    }
#endif
#ifdef LIU_WHISPER_VULKAN
    /* Metal first where it exists — it is the tuned path on Apple hardware,
     * and MoltenVK would only translate the SPIR-V back into Metal. */
    if (!e->backend) {
        const char *force = getenv("LIU_WHISPER_FORCE_CPU");
        if (!(force && force[0] && force[0] != '0')) {
            e->vk = whisper_vulkan_create(&e->weights, NULL, 0);
            if (e->vk) e->backend = "vulkan";
        }
    }
#endif
    if (!e->backend) {
        e->cpu = whisper_cpu_create(&e->weights, 0, err, err_cap);
        if (!e->cpu) { whisper_engine_free(e); return NULL; }
        e->backend = "cpu";
    }
    whisper_refresh_residency(&e->file);
    return e;
}

/* ---- backend dispatch --------------------------------------------------- */

static bool we_encode(WhisperEngine *e, const f32 *mel) {
#ifdef LIU_WHISPER_METAL
    if (e->mtl) return whisper_metal_encode(e->mtl, mel);
#endif
#ifdef LIU_WHISPER_VULKAN
    if (e->vk) return whisper_vulkan_encode(e->vk, mel);
#endif
    whisper_cpu_set_cancel(e->cpu, we_cancelled, e);
    return whisper_cpu_encode(e->cpu, mel);
}

static bool we_decode(WhisperEngine *e, const i32 *tok, i32 n, i32 n_past) {
#ifdef LIU_WHISPER_METAL
    if (e->mtl) return whisper_metal_decode(e->mtl, tok, n, n_past, e->logits);
#endif
#ifdef LIU_WHISPER_VULKAN
    if (e->vk) return whisper_vulkan_decode(e->vk, tok, n, n_past, e->logits);
#endif
    return whisper_cpu_decode(e->cpu, tok, n, n_past, e->logits);
}

static void we_decode_reset(WhisperEngine *e) {
#ifdef LIU_WHISPER_METAL
    if (e->mtl) { whisper_metal_decode_reset(e->mtl); return; }
#endif
#ifdef LIU_WHISPER_VULKAN
    if (e->vk) { whisper_vulkan_decode_reset(e->vk); return; }
#endif
    whisper_cpu_decode_reset(e->cpu);
}

/* ---- logit rules -------------------------------------------------------- */

typedef struct {
    i32  seek_delta;       /* mel frames the window will advance by  */
    bool has_ts;           /* a timestamp has been emitted           */
    i32  n_gen;            /* generated tokens so far (excl. prompt) */
    i32  last, penult;     /* last two generated ids, -1 when absent */
} WeDecodeState;

static void we_apply_rules(WhisperEngine *e, const WeDecodeState *st, i32 max_tokens) {
    const WhisperVocab *v = &e->vocab;
    f32 *lg = e->logits;
    const i32 n = v->n_vocab;

    const bool is_initial = (st->n_gen == 0);

    if (is_initial) {
        lg[v->token_eot] = -INFINITY;
        /* Token 220 is a bare space in every Whisper vocabulary; opening a
         * transcript with it is Whisper's classic empty-audio artefact. */
        if (220 < n) lg[220] = -INFINITY;
    }

    lg[v->token_not]        = -INFINITY;   /* timestamps stay on */
    lg[v->token_sot]        = -INFINITY;
    lg[v->token_nosp]       = -INFINITY;
    lg[v->token_solm]       = -INFINITY;
    lg[v->token_translate]  = -INFINITY;
    lg[v->token_transcribe] = -INFINITY;
    lg[v->token_prev]       = -INFINITY;
    for (i32 i = 0; i < v->n_languages; i++) {
        i32 t = v->token_sot + 1 + i;
        if (t < n) lg[t] = -INFINITY;
    }

    /* Past the token budget, only a timestamp (or EOT) may close the window,
     * so the segment stays well-formed instead of being truncated raw. */
    if (max_tokens > 0 && st->n_gen >= max_tokens) {
        for (i32 i = 0; i < v->token_eot; i++) lg[i] = -INFINITY;
    }

    /* Timestamps in pairs: after one timestamp the model owes text; after
     * text that followed a timestamp it owes the closing one. */
    {
        const bool last_was_ts   = st->last >= 0 && st->last >= v->token_beg;
        const bool penult_was_ts = st->n_gen < 2 || (st->penult >= v->token_beg);
        if (last_was_ts) {
            if (penult_was_ts) {
                for (i32 i = v->token_beg; i < n; i++) lg[i] = -INFINITY;
            } else {
                for (i32 i = 0; i < v->token_eot; i++) lg[i] = -INFINITY;
            }
        }
    }

    /* The opening timestamp cannot be later than 1 s into the window. */
    if (is_initial) {
        const f32 precision = (f32)WHISPER_CHUNK_SEC / (f32)e->file.h.n_audio_ctx;
        const i32 tid0 = (i32)lroundf(WE_MAX_INITIAL_TS_SEC / precision);
        for (i32 i = v->token_beg + tid0 + 1; i < n; i++) lg[i] = -INFINITY;
    }

    /* Timestamps never go backwards inside a window. */
    if (st->has_ts) {
        const i32 tid0 = st->seek_delta / 2;
        for (i32 i = v->token_beg; i < v->token_beg + tid0 && i < n; i++) lg[i] = -INFINITY;
    }

    /* If the timestamps' total probability beats the best text token, force
     * a timestamp. Computed in log space over the already-masked logits. */
    {
        f32 lmax = -INFINITY;
        for (i32 i = 0; i < n; i++) if (lg[i] > lmax) lmax = lg[i];
        if (!isfinite(lmax)) return;

        f64 sum = 0;
        for (i32 i = 0; i < n; i++) if (lg[i] > -INFINITY) sum += exp((f64)(lg[i] - lmax));
        if (sum <= 0) return;
        const f64 logZ = log(sum) + (f64)lmax;

        f64 ts_mass = 0;
        for (i32 i = v->token_beg; i < n; i++) {
            if (lg[i] > -INFINITY) ts_mass += exp((f64)(lg[i] - lmax));
        }
        f32 best_text = -INFINITY;
        for (i32 i = 0; i < v->token_beg; i++) if (lg[i] > best_text) best_text = lg[i];

        if (ts_mass > 0) {
            const f64 ts_logprob   = log(ts_mass) + (f64)lmax - logZ;
            const f64 text_logprob = (f64)best_text - logZ;
            if (ts_logprob > text_logprob) {
                for (i32 i = 0; i < v->token_beg; i++) lg[i] = -INFINITY;
            }
        }
    }
}

static i32 we_argmax(const f32 *lg, i32 n) {
    i32 best = 0;
    f32 bv = -INFINITY;
    for (i32 i = 0; i < n; i++) if (lg[i] > bv) { bv = lg[i]; best = i; }
    return best;
}

/* ---- language detection ------------------------------------------------- */

/* One decoder step from a bare <|startoftranscript|>; the language token
 * with the highest logit wins. Requires an encode to have run. */
static bool we_detect_language(WhisperEngine *e, char *out, usize cap) {
    const WhisperVocab *v = &e->vocab;
    if (!v->multilingual) { snprintf(out, cap, "en"); return true; }

    we_decode_reset(e);
    i32 sot = v->token_sot;
    if (!we_decode(e, &sot, 1, 0)) return false;

    i32 best = -1;
    f32 bv = -INFINITY;
    for (i32 i = 0; i < v->n_languages; i++) {
        const i32 t = v->token_sot + 1 + i;
        if (t >= v->n_vocab) break;
        if (e->logits[t] > bv) { bv = e->logits[t]; best = i; }
    }
    const char *code = whisper_vocab_lang_code(best);
    snprintf(out, cap, "%s", code ? code : "en");
    return true;
}

/* ---- transcription ------------------------------------------------------ */

i32 whisper_engine_transcribe(WhisperEngine *e,
                              const f32 *samples, i32 n_samples,
                              const WhisperParams *p,
                              char *out, i32 out_cap) {
    if (!e || !out || out_cap <= 0) return -1;
    out[0] = '\0';
    if (n_samples <= 0) return 0;
    e->cancelled = false;
    e->progress_fraction = 0;
    if (we_cancelled(e)) return 0;

    WhisperParams pp = p ? *p : whisper_params_default();
    const WhisperVocab *v = &e->vocab;

    WhisperMel mel;
    if (!whisper_mel_compute(samples, n_samples, e->file.filters,
                             e->file.n_mel, e->file.n_fft, &mel)) return -1;

    /* Frames backed by real audio; the transform's 30 s silence tail is not
     * worth another encoder pass. */
    const i32 seek_end = mel.n_len_org > 0 ? mel.n_len_org : 1;

    i32 max_tokens = pp.max_tokens;
    if (max_tokens <= 0) max_tokens = e->file.h.n_text_ctx / 2;

    i32 out_len = 0;
    i32 seek = 0;
    bool first_window = true;

    while (seek + WE_DELTA_MIN < seek_end) {
        e->progress_fraction = (f32)seek / (f32)seek_end;
        if (we_cancelled(e)) break;
        if (!whisper_mel_window(&mel, seek, e->mel_win)) break;
        if (!we_encode(e, e->mel_win)) {
            if (e->cancelled) goto done;
            whisper_mel_free(&mel); return -1;
        }
        if (we_cancelled(e)) break;

        if (first_window) {
            if (pp.lang[0]) {
                snprintf(e->language, sizeof e->language, "%s", pp.lang);
            } else if (!we_detect_language(e, e->language, sizeof e->language)) {
                whisper_mel_free(&mel);
                return -1;
            }
            first_window = false;
        }

        /* Prompt: <|sot|> <|lang|> <|transcribe|/|translate|>. Timestamps
         * are left enabled (no <|notimestamps|>) because the seek below
         * depends on them. */
        i32 n_prompt = 0;
        /* whisper_ggml.c refuses n_text_ctx < 8, so the three prompt slots
         * always fit; this is the local statement of that contract. */
        if (e->file.h.n_text_ctx < 4) { whisper_mel_free(&mel); return -1; }
        e->tokens[n_prompt++] = v->token_sot;
        if (v->multilingual) {
            i32 lt = whisper_vocab_lang_token(v, e->language);
            if (lt < 0) lt = whisper_vocab_lang_token(v, "en");
            if (lt >= 0) e->tokens[n_prompt++] = lt;
            /* Never hand <|translate|> to a model that was not trained on the
             * task. It does not refuse — it emits degenerate output — so
             * asking anyway turns a working dictation into a lost one.
             * Transcribing instead is the graceful degradation: the words are
             * still right, they are just not in English, and
             * whisper_engine_can_translate() lets the UI say so before the
             * user records. */
            const bool xlate = pp.translate && whisper_engine_can_translate(e);
            e->tokens[n_prompt++] = xlate ? v->token_translate
                                          : v->token_transcribe;
        }

        we_decode_reset(e);
        if (!we_decode(e, e->tokens, n_prompt, 0)) { whisper_mel_free(&mel); return -1; }

        WeDecodeState st = { WE_FULL_WINDOW, false, 0, -1, -1 };

        /* Text is staged per window so a segment the model abandons (it
         * emitted no closing timestamp) can be dropped wholesale rather than
         * half-written into the caller's buffer. */
        i32 committed = out_len;
        i32 n_past = n_prompt;

        const i32 n_max = e->file.h.n_text_ctx - n_prompt - 1;
        for (i32 i = 0; i < n_max; i++) {
            if (we_cancelled(e)) { out_len = committed; goto done; }
            we_apply_rules(e, &st, max_tokens);
            const i32 id = we_argmax(e->logits, v->n_vocab);

            if (id == v->token_eot) break;

            if (id > v->token_beg) {
                const i32 delta = 2 * (id - v->token_beg);
                /* A timestamp that moves backwards means the decoder has
                 * gone astray; keep what is committed and force the window
                 * to advance by the seek delta already reached. */
                if (st.has_ts && delta < st.seek_delta) break;
                st.seek_delta = delta;
                st.has_ts = true;
                committed = out_len;      /* segment closed: text is safe */
                /* This timestamp reaches the end of the audio, so the
                 * window is done even though the model has not emitted
                 * EOT — letting it keep going only invites it to narrate
                 * the silence tail. */
                if (seek + st.seek_delta + WE_DELTA_MIN >= seek_end) break;
            } else if (!whisper_vocab_is_special(v, id)) {
                if (!whisper_vocab_append(v, id, out, &out_len, out_cap - 1)) {
                    /* Caller's buffer is full — stop cleanly with what fits. */
                    committed = out_len;
                    st.has_ts = true;
                    goto done;
                }
            }

            st.penult = st.last;
            st.last   = id;
            st.n_gen++;

            if (n_past >= e->file.h.n_text_ctx) break;
            if (!we_decode(e, &id, 1, n_past)) { whisper_mel_free(&mel); return -1; }
            n_past++;
        }

        /* Whatever was not closed by a timestamp is rolled back. */
        out_len = st.has_ts ? committed : out_len;

        i32 advance = st.has_ts ? st.seek_delta : WE_FULL_WINDOW;

        /* A sequence that ends "text, timestamp" means the model closed its
         * last segment and had nothing more to say about this window, so the
         * REST of the window is skipped rather than re-encoded from that
         * timestamp. Without this the leftover fraction of a second gets its
         * own pass and Whisper narrates the silence — this is where the
         * stray "you" at the end of a clean recording comes from, not from
         * the acoustic front end. */
        const bool single_ts_ending =
            st.n_gen > 1 && st.penult < v->token_beg && st.last > v->token_beg;
        if (single_ts_ending) advance = seek_end - seek;

        if (advance < 1) advance = WE_FULL_WINDOW;
        if (advance > WE_FULL_WINDOW) advance = WE_FULL_WINDOW;
        seek += advance;

        if (e->progress) {
            const f32 frac = seek_end > 0 ? (f32)seek / (f32)seek_end : 1.0f;
            if (!e->progress(e->progress_user, frac < 1.0f ? frac : 1.0f)) break;
        }
    }

done:
    whisper_mel_free(&mel);
    whisper_refresh_residency(&e->file);
    out[out_len] = '\0';
    return out_len;
}
