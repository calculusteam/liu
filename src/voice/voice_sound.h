/*
 * voice_sound.h — the two sounds a dictation makes: the microphone opening, and
 * closing.
 *
 * Two short sine blips, each bent a little upward so that neither is a note —
 * the lower then the higher for "listening", the higher then the lower for
 * "done".
 *
 * Pure C, no player: voice_sound_render() fills a buffer and whoever owns an
 * output (the app: platform_audio_play_clip) plays it.
 */
#ifndef LIU_VOICE_SOUND_H
#define LIU_VOICE_SOUND_H

#include "core/types.h"

#define VOICE_SOUND_RATE     48000
#define VOICE_SOUND_FRAMES   9600            /* 0.2 s */
/* Both sounds peak here (-16 dBFS). */
#define VOICE_SOUND_PEAK     0.15848931924611134f

typedef enum {
    VOICE_SOUND_ON = 0,      /* the microphone opened: the lower blip, then the higher */
    VOICE_SOUND_OFF,         /* it closed: the higher, then the lower */
    VOICE_SOUND_COUNT
} VoiceSound;

/* Fill out[VOICE_SOUND_FRAMES] with one sound: mono, 48 kHz, within +-1. */
void voice_sound_render(VoiceSound which, f32 *out);

/* Blip `i` (0 or 1) starts at `*first` and lasts `*count` frames, with
 * frequency changing from `*f0` to `*f1` Hz. Samples outside the two blips
 * are zero. */
void voice_sound_blip(VoiceSound which, i32 i, i32 *first, i32 *count, f64 *f0, f64 *f1);

#endif /* LIU_VOICE_SOUND_H */
