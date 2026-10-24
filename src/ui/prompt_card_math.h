/*
 * Liu — the prompt card's attachment-strip arithmetic, apart from the screen.
 *
 * Same arrangement as ui/voice_hud_math.h and ui/agent_busy.h, for the same
 * reason: the part of this feature that actually breaks is a layout rule, and a
 * layout rule verified by looking at it is verified once. What is here is the
 * whole of "how many tiles fit, how wide is each, and what happens to the ones
 * that do not" — no renderer, no AppState, no image decoding. The caller
 * supplies each attachment's ASPECT (width / height) because that is all the
 * layout needs from a picture; resolving a path to pixels is prompt_thumb.c's
 * job and needs a disk.
 *
 * That includes the case the eye cannot check: an overflow tile has to be able
 * to push already-placed tiles back out of the row to make room for itself, or
 * a strip silently shows 2 of 4 attachments and says nothing about the other
 * two.
 */
#ifndef UI_PROMPT_CARD_MATH_H
#define UI_PROMPT_CARD_MATH_H

#include "core/types.h"

/* One tile, positioned relative to the strip's left edge. `img` indexes the
 * prompt's attachment array; -1 marks the trailing "+N" overflow tile. */
typedef struct { f32 x, y, w, h; i32 img; } PromptTile;

/* A tile is as wide as its picture's aspect wants, between the two clamps.
 *
 * Squares are wrong for this content: what a user attaches to a coding agent
 * is overwhelmingly a screenshot, and a 16:9 window centre-cropped to a square
 * is unrecognisable at tile size — the one thing the tile exists to do.
 * `aspect` <= 0 means the picture could not be decoded; that gets a square,
 * which is also the shape that reads as "no picture here". */
static inline f32 prompt_tile_w(f32 aspect, f32 h, f32 min_w, f32 max_w) {
    f32 w = (aspect > 0.0f) ? h * aspect : h;
    if (w < min_w) w = min_w;
    if (w > max_w) w = max_w;
    return w;
}

/* Lay a row of tiles into `inner_w`, left to right.
 *
 * `aspects` holds `n_imgs` width/height ratios (<= 0 for an image that could
 * not be decoded). `extra` is the count of attachments the prompt carried past
 * its own storage cap, which is already lost as paths but must not be lost as a
 * NUMBER.
 *
 * Whatever does not fit is folded into a trailing "+N" tile, and the fold is
 * allowed to evict tiles that were already placed in order to make room for
 * itself. The eviction is required: a strip that quietly showed the first two
 * of four attachments would be misleading, where one showing one picture and
 * "+3" is not.
 *
 * Returns the number of tiles written; the strip's total width lands in *out_w.
 * `out_hidden`, when given, receives how many attachments the "+N" tile stands
 * for (0 when every one of them is on screen). */
static inline i32 prompt_tiles_layout(const f32 *aspects, i32 n_imgs, i32 extra,
                                      f32 h, f32 min_w, f32 max_w, f32 gap,
                                      f32 inner_w, PromptTile *out, i32 max_out,
                                      f32 *out_w, i32 *out_hidden) {
    if (out_w) *out_w = 0.0f;
    if (out_hidden) *out_hidden = 0;
    if (!out || max_out <= 0 || inner_w <= 0.0f || h <= 0.0f) return 0;
    if (n_imgs < 0) n_imgs = 0;
    if (extra  < 0) extra  = 0;

    i32 n = 0;
    f32 x = 0.0f;                       /* left edge of the NEXT tile */
    for (i32 i = 0; i < n_imgs && n < max_out; i++) {
        f32 w = prompt_tile_w(aspects ? aspects[i] : 0.0f, h, min_w, max_w);
        if (x + w > inner_w) break;
        out[n].x = x; out[n].y = 0.0f; out[n].w = w; out[n].h = h;
        out[n].img = i;
        x += w + gap;
        n++;
    }

    i32 hidden = (n_imgs - n) + extra;
    if (hidden > 0) {
        /* Evict from the right until the overflow tile fits — by WIDTH, and by
         * SLOT. The slot half is not theoretical: a caller whose array is
         * exactly as long as the row it laid out has no room left for the
         * marker, and the first version answered that by reporting an empty
         * strip. Each eviction adds one to what the marker stands for, so the
         * count stays exact however many rounds this takes. */
        while (n > 0 && (x + min_w > inner_w || n >= max_out)) {
            n--;
            x -= out[n].w + gap;
            hidden++;
        }
        if (x + min_w <= inner_w && n < max_out) {
            out[n].x = x; out[n].y = 0.0f; out[n].w = min_w; out[n].h = h;
            out[n].img = -1;
            x += min_w + gap;
            n++;
            if (out_hidden) *out_hidden = hidden;
        } else {
            /* No room even for the marker: report nothing rather than a strip
             * claiming to be complete. */
            n = 0;
            x = 0.0f;
        }
    }
    if (out_w) *out_w = (n > 0) ? x - gap : 0.0f;
    return n;
}

#endif /* UI_PROMPT_CARD_MATH_H */
