/*
 * Liu - Whisper weight table.
 *
 * Every lookup goes through need()/need_fmt(), which fail loudly with the
 * offending tensor name rather than returning NULL for the forward pass to
 * dereference. Shapes are checked here too: the encoder and decoder share a
 * state width in Whisper, and a file where they disagree would otherwise
 * corrupt the cross-attention strides only.
 */
#include "voice/whisper_model.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const WhisperFile *f;
    char  *err;
    usize  err_cap;
    bool   ok;
} Resolver;

static void res_fail(Resolver *r, const char *what, const char *name) {
    if (r->ok && r->err && r->err_cap) snprintf(r->err, r->err_cap, "%s: %s", what, name);
    r->ok = false;
}

static const WhisperTensor *need(Resolver *r, const char *name) {
    if (!r->ok) return NULL;
    const WhisperTensor *t = whisper_tensor(r->f, name);
    if (!t) res_fail(r, "missing tensor", name);
    return t;
}

static const WhisperTensor *need_fmt(Resolver *r, const char *fmt, i32 layer) {
    if (!r->ok) return NULL;
    char name[WHISPER_TENSOR_NAME_CAP];
    if (snprintf(name, sizeof name, fmt, layer) >= (int)sizeof name) {
        res_fail(r, "tensor name too long", fmt);
        return NULL;
    }
    const WhisperTensor *t = whisper_tensor(r->f, name);
    if (!t) res_fail(r, "missing tensor", name);
    return t;
}

/* Assert ne[0]/ne[1]; pass -1 for an axis that is free (and reported back
 * through `free_axis` when non-NULL). */
static void want_2d(Resolver *r, const WhisperTensor *t, i32 ne0, i32 ne1, i32 *free_axis) {
    if (!r->ok || !t) return;
    if (ne0 >= 0 && t->ne[0] != ne0) { res_fail(r, "wrong shape", t->name); return; }
    if (ne1 >= 0 && t->ne[1] != ne1) { res_fail(r, "wrong shape", t->name); return; }
    if (free_axis) *free_axis = (ne0 < 0) ? t->ne[0] : t->ne[1];
}

/* A bias/gain vector of `n` elements, however the converter spelled its
 * shape. Whisper's convolution biases are written as ne = [1, n_state]
 * (ggml broadcasts them over time) while every other vector is ne = [n], so
 * a strict ne[0] == n check rejects a perfectly good model. The dequantizer
 * reads these linearly, so any shape with n elements and no real second
 * axis is equivalent. */
static void want_vec(Resolver *r, const WhisperTensor *t, i32 n) {
    if (!r->ok || !t) return;
    i64 total = 1;
    for (i32 d = 0; d < t->n_dims; d++) total *= t->ne[d];
    if (total != n) res_fail(r, "wrong shape", t->name);
}

/* One attention block's tensors. `pfx_*` are the printf templates for this
 * block's family (self-attn vs cross-attn share the shape rules). */
static void load_attn(Resolver *r, i32 il, i32 n_state,
                      const char *ln_w, const char *ln_b,
                      const char *q_w,  const char *q_b,
                      const char *k_w,
                      const char *v_w,  const char *v_b,
                      const char *o_w,  const char *o_b,
                      const WhisperTensor **out) {
    out[0] = need_fmt(r, ln_w, il); want_vec(r, out[0], n_state);
    out[1] = need_fmt(r, ln_b, il); want_vec(r, out[1], n_state);
    out[2] = need_fmt(r, q_w,  il); want_2d(r, out[2], n_state, n_state, NULL);
    out[3] = need_fmt(r, q_b,  il); want_vec(r, out[3], n_state);
    out[4] = need_fmt(r, k_w,  il); want_2d(r, out[4], n_state, n_state, NULL);
    out[5] = need_fmt(r, v_w,  il); want_2d(r, out[5], n_state, n_state, NULL);
    out[6] = need_fmt(r, v_b,  il); want_vec(r, out[6], n_state);
    out[7] = need_fmt(r, o_w,  il); want_2d(r, out[7], n_state, n_state, NULL);
    out[8] = need_fmt(r, o_b,  il); want_vec(r, out[8], n_state);
}

static void load_mlp(Resolver *r, WhisperLayer *L, i32 il, i32 n_state,
                     const char *prefix, i32 *n_ff) {
    char f[WHISPER_TENSOR_NAME_CAP];

    snprintf(f, sizeof f, "%s.blocks.%%d.mlp_ln.weight", prefix);
    L->mlp_ln_w = need_fmt(r, f, il); want_vec(r, L->mlp_ln_w, n_state);
    snprintf(f, sizeof f, "%s.blocks.%%d.mlp_ln.bias", prefix);
    L->mlp_ln_b = need_fmt(r, f, il); want_vec(r, L->mlp_ln_b, n_state);

    snprintf(f, sizeof f, "%s.blocks.%%d.mlp.0.weight", prefix);
    L->mlp_0_w = need_fmt(r, f, il); want_2d(r, L->mlp_0_w, n_state, -1, n_ff);
    snprintf(f, sizeof f, "%s.blocks.%%d.mlp.0.bias", prefix);
    L->mlp_0_b = need_fmt(r, f, il); want_vec(r, L->mlp_0_b, *n_ff);

    snprintf(f, sizeof f, "%s.blocks.%%d.mlp.2.weight", prefix);
    L->mlp_2_w = need_fmt(r, f, il); want_2d(r, L->mlp_2_w, *n_ff, n_state, NULL);
    snprintf(f, sizeof f, "%s.blocks.%%d.mlp.2.bias", prefix);
    L->mlp_2_b = need_fmt(r, f, il); want_vec(r, L->mlp_2_b, n_state);
}

void whisper_weights_free(WhisperWeights *w) {
    if (!w) return;
    free(w->enc);
    free(w->dec);
    memset(w, 0, sizeof *w);
}

bool whisper_weights_load(WhisperWeights *out, const WhisperFile *f,
                          char *err, usize err_cap) {
    if (!out || !f) return false;
    memset(out, 0, sizeof *out);
    if (err && err_cap) err[0] = '\0';

    out->f = f;
    out->h = f->h;

    const i32 n_astate = f->h.n_audio_state;
    const i32 n_tstate = f->h.n_text_state;

    /* Whisper ties the two widths; the cross-attention projections read the
     * encoder output with the decoder's stride, so a mismatch is fatal. */
    if (n_astate != n_tstate) {
        if (err && err_cap) snprintf(err, err_cap, "encoder/decoder state width mismatch");
        return false;
    }

    out->n_audio_head_dim = n_astate / f->h.n_audio_head;
    out->n_text_head_dim  = n_tstate / f->h.n_text_head;

    out->enc = calloc((usize)f->h.n_audio_layer, sizeof *out->enc);
    out->dec = calloc((usize)f->h.n_text_layer,  sizeof *out->dec);
    if (!out->enc || !out->dec) {
        if (err && err_cap) snprintf(err, err_cap, "out of memory");
        whisper_weights_free(out);
        return false;
    }

    Resolver r = { f, err, err_cap, true };

    /* ---- encoder stem --------------------------------------------------- */
    out->e_conv1_w = need(&r, "encoder.conv1.weight");
    out->e_conv1_b = need(&r, "encoder.conv1.bias");
    out->e_conv2_w = need(&r, "encoder.conv2.weight");
    out->e_conv2_b = need(&r, "encoder.conv2.bias");
    if (r.ok) {
        /* [kernel, in_ch, out_ch] */
        if (out->e_conv1_w->ne[0] != 3 || out->e_conv1_w->ne[1] != f->h.n_mels ||
            out->e_conv1_w->ne[2] != n_astate)
            res_fail(&r, "wrong shape", "encoder.conv1.weight");
        if (out->e_conv2_w->ne[0] != 3 || out->e_conv2_w->ne[1] != n_astate ||
            out->e_conv2_w->ne[2] != n_astate)
            res_fail(&r, "wrong shape", "encoder.conv2.weight");
    }
    want_vec(&r, out->e_conv1_b, n_astate);
    want_vec(&r, out->e_conv2_b, n_astate);

    out->e_pos = need(&r, "encoder.positional_embedding");
    want_2d(&r, out->e_pos, n_astate, f->h.n_audio_ctx, NULL);

    out->e_ln_w = need(&r, "encoder.ln_post.weight"); want_vec(&r, out->e_ln_w, n_astate);
    out->e_ln_b = need(&r, "encoder.ln_post.bias");   want_vec(&r, out->e_ln_b, n_astate);

    /* ---- encoder layers ------------------------------------------------- */
    for (i32 il = 0; il < f->h.n_audio_layer && r.ok; il++) {
        WhisperLayer *L = &out->enc[il];
        const WhisperTensor *a[9];
        load_attn(&r, il, n_astate,
                  "encoder.blocks.%d.attn_ln.weight", "encoder.blocks.%d.attn_ln.bias",
                  "encoder.blocks.%d.attn.query.weight", "encoder.blocks.%d.attn.query.bias",
                  "encoder.blocks.%d.attn.key.weight",
                  "encoder.blocks.%d.attn.value.weight", "encoder.blocks.%d.attn.value.bias",
                  "encoder.blocks.%d.attn.out.weight",   "encoder.blocks.%d.attn.out.bias",
                  a);
        L->attn_ln_w = a[0]; L->attn_ln_b = a[1];
        L->attn_q_w  = a[2]; L->attn_q_b  = a[3];
        L->attn_k_w  = a[4];
        L->attn_v_w  = a[5]; L->attn_v_b  = a[6];
        L->attn_o_w  = a[7]; L->attn_o_b  = a[8];

        i32 ff = 0;
        load_mlp(&r, L, il, n_astate, "encoder", &ff);
        if (il == 0) out->n_audio_ff = ff;
        else if (ff != out->n_audio_ff) res_fail(&r, "inconsistent ff width", "encoder.mlp.0.weight");
    }

    /* ---- decoder stem --------------------------------------------------- */
    out->d_tok_emb = need(&r, "decoder.token_embedding.weight");
    want_2d(&r, out->d_tok_emb, n_tstate, f->h.n_vocab, NULL);
    out->d_pos = need(&r, "decoder.positional_embedding");
    want_2d(&r, out->d_pos, n_tstate, f->h.n_text_ctx, NULL);
    out->d_ln_w = need(&r, "decoder.ln.weight"); want_vec(&r, out->d_ln_w, n_tstate);
    out->d_ln_b = need(&r, "decoder.ln.bias");   want_vec(&r, out->d_ln_b, n_tstate);

    /* ---- decoder layers ------------------------------------------------- */
    for (i32 il = 0; il < f->h.n_text_layer && r.ok; il++) {
        WhisperLayer *L = &out->dec[il];
        const WhisperTensor *a[9];

        load_attn(&r, il, n_tstate,
                  "decoder.blocks.%d.attn_ln.weight", "decoder.blocks.%d.attn_ln.bias",
                  "decoder.blocks.%d.attn.query.weight", "decoder.blocks.%d.attn.query.bias",
                  "decoder.blocks.%d.attn.key.weight",
                  "decoder.blocks.%d.attn.value.weight", "decoder.blocks.%d.attn.value.bias",
                  "decoder.blocks.%d.attn.out.weight",   "decoder.blocks.%d.attn.out.bias",
                  a);
        L->attn_ln_w = a[0]; L->attn_ln_b = a[1];
        L->attn_q_w  = a[2]; L->attn_q_b  = a[3];
        L->attn_k_w  = a[4];
        L->attn_v_w  = a[5]; L->attn_v_b  = a[6];
        L->attn_o_w  = a[7]; L->attn_o_b  = a[8];

        load_attn(&r, il, n_tstate,
                  "decoder.blocks.%d.cross_attn_ln.weight", "decoder.blocks.%d.cross_attn_ln.bias",
                  "decoder.blocks.%d.cross_attn.query.weight", "decoder.blocks.%d.cross_attn.query.bias",
                  "decoder.blocks.%d.cross_attn.key.weight",
                  "decoder.blocks.%d.cross_attn.value.weight", "decoder.blocks.%d.cross_attn.value.bias",
                  "decoder.blocks.%d.cross_attn.out.weight",   "decoder.blocks.%d.cross_attn.out.bias",
                  a);
        L->cross_ln_w = a[0]; L->cross_ln_b = a[1];
        L->cross_q_w  = a[2]; L->cross_q_b  = a[3];
        L->cross_k_w  = a[4];
        L->cross_v_w  = a[5]; L->cross_v_b  = a[6];
        L->cross_o_w  = a[7]; L->cross_o_b  = a[8];

        i32 ff = 0;
        load_mlp(&r, L, il, n_tstate, "decoder", &ff);
        if (il == 0) out->n_text_ff = ff;
        else if (ff != out->n_text_ff) res_fail(&r, "inconsistent ff width", "decoder.mlp.0.weight");
    }

    if (!r.ok) { whisper_weights_free(out); return false; }
    return true;
}
