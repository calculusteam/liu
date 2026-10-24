/*
 * Liu - Whisper weight table: every tensor the forward pass needs, resolved
 * and shape-checked once at load.
 *
 * Both backends (CPU reference and Metal) consume this. Resolution is
 * separated from execution because the tensor names are the one part of the
 * model that is pure bookkeeping — getting `encoder.blocks.%d.attn.key.weight`
 * wrong is a load-time error here rather than a wrong number 40 layers deep.
 *
 * Shape conventions, in ggml order (ne[0] is the fastest-moving axis):
 *   - A projection weight is ne = [K, N]: row n of the matrix is N
 *     contiguous... no, K contiguous elements at offset n*K. The matmul it
 *     feeds is y[n] = dot(x, W_row_n), i.e. an implicit transpose.
 *   - conv weights are ne = [kernel, in_channels, out_channels].
 *   - The K projection has NO bias in Whisper — self- and cross-attention
 *     both. A backend that adds one silently detunes every attention score.
 */
#ifndef VOICE_WHISPER_MODEL_H
#define VOICE_WHISPER_MODEL_H

#include "core/types.h"
#include "voice/whisper_ggml.h"

typedef struct {
    const WhisperTensor *attn_ln_w,  *attn_ln_b;
    const WhisperTensor *attn_q_w,   *attn_q_b;
    const WhisperTensor *attn_k_w;                 /* no bias, by design */
    const WhisperTensor *attn_v_w,   *attn_v_b;
    const WhisperTensor *attn_o_w,   *attn_o_b;

    /* Decoder only; NULL for encoder layers. */
    const WhisperTensor *cross_ln_w, *cross_ln_b;
    const WhisperTensor *cross_q_w,  *cross_q_b;
    const WhisperTensor *cross_k_w;                /* no bias, by design */
    const WhisperTensor *cross_v_w,  *cross_v_b;
    const WhisperTensor *cross_o_w,  *cross_o_b;

    const WhisperTensor *mlp_ln_w,   *mlp_ln_b;
    const WhisperTensor *mlp_0_w,    *mlp_0_b;
    const WhisperTensor *mlp_2_w,    *mlp_2_b;
} WhisperLayer;

typedef struct {
    const WhisperFile *f;          /* borrowed; must outlive the weights */
    WhisperHParams     h;

    i32 n_audio_head_dim;          /* n_audio_state / n_audio_head */
    i32 n_text_head_dim;           /* n_text_state  / n_text_head  */
    i32 n_audio_ff;                /* from encoder mlp.0 shape     */
    i32 n_text_ff;

    const WhisperTensor *e_conv1_w, *e_conv1_b;
    const WhisperTensor *e_conv2_w, *e_conv2_b;
    const WhisperTensor *e_pos;                    /* [n_state, n_audio_ctx] */
    const WhisperTensor *e_ln_w,    *e_ln_b;

    const WhisperTensor *d_tok_emb;                /* [n_state, n_vocab], tied LM head */
    const WhisperTensor *d_pos;                    /* [n_state, n_text_ctx]  */
    const WhisperTensor *d_ln_w,    *d_ln_b;

    WhisperLayer *enc;             /* heap [n_audio_layer] */
    WhisperLayer *dec;             /* heap [n_text_layer]  */
} WhisperWeights;

/* Resolve every tensor and validate its shape against the header. Returns
 * false with a reason in `err` on the first missing or mis-shaped tensor. */
bool whisper_weights_load(WhisperWeights *out, const WhisperFile *f,
                          char *err, usize err_cap);
void whisper_weights_free(WhisperWeights *w);

#endif /* VOICE_WHISPER_MODEL_H */
