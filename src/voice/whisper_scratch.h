#ifndef LIU_WHISPER_SCRATCH_H
#define LIU_WHISPER_SCRATCH_H
#include "core/checked.h"

/* FF storage is shared by encoder MLP, decoder MLP and convolution. */
static inline bool whisper_scratch_bytes(i32 audio_rows, i32 text_rows,
                                         i32 audio_ff, i32 text_ff,
                                         i32 state, i32 frames, usize *bytes) {
    if (!bytes || audio_rows <= 0 || text_rows <= 0 || audio_ff <= 0 ||
        text_ff <= 0 || state <= 0 || frames <= 0) return false;
    usize a, t, conv;
    if (!checked_mul_usize((usize)audio_rows, (usize)audio_ff, &a) ||
        !checked_mul_usize((usize)text_rows, (usize)text_ff, &t) ||
        !checked_mul_usize((usize)state, (usize)frames, &conv)) return false;
    usize elements = a > t ? a : t;
    if (conv > elements) elements = conv;
    return checked_mul_usize(elements, sizeof(f32), bytes);
}
#endif
