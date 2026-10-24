/*
 * Voice input captures speech and inserts the transcript into the active
 * agent pane. Supported agent CLIs receive ordinary text, so the
 * transcription path is independent of the agent.
 *
 * The session state machine and capture pump run on the main thread; one
 * worker loads the model and transcribes audio. Loading begins when recording
 * starts, overlapping model setup with capture. Local Whisper is the default
 * and keeps audio on device. The API backend sends recorded audio to a
 * configured speech-to-text endpoint. Both backends share the same session
 * state, auto-stop rules, and transcript handoff.
 */
#ifndef VOICE_VOICE_H
#define VOICE_VOICE_H

#include "core/types.h"

typedef enum {
    VOICE_IDLE = 0,
    VOICE_WAITING_MIC,    /* permission prompt is up; capture starts on grant */
    VOICE_RECORDING,
    VOICE_TRANSCRIBING,
    VOICE_DONE,           /* transcript is waiting in voice_take_text()      */
    VOICE_ERROR,
} VoiceState;

typedef enum {
    VOICE_BACKEND_LOCAL = 0,   /* on-device Whisper; audio never leaves the Mac */
    VOICE_BACKEND_API   = 1,   /* hosted speech-to-text over HTTPS              */
} VoiceBackend;

typedef struct VoiceConfig {
    bool enabled;
    VoiceBackend backend;
    char model_path[512];       /* whisper.cpp-format ggml-*.bin              */
    char lang[8];               /* ISO code; empty auto-detects               */
    bool translate;             /* emit English regardless of what was spoken */

    /* API backend. `api_provider` is one of "openai" | "groq" | "elevenlabs" |
     * "custom". `api_key` may be empty: the request then falls back to the
     * provider's conventional env var (OPENAI_API_KEY / GROQ_API_KEY /
     * ELEVENLABS_API_KEY). `api_base_url` is consulted for "custom" only — any
     * OpenAI-compatible /audio/transcriptions server (a local whisper.cpp
     * server, LM Studio, a self-hosted gateway). Anthropic is deliberately
     * absent: it has no speech-to-text API. */
    char api_provider[16];
    char api_model[64];
    char api_key[256];
    char api_base_url[256];

    /* Persistent UID of the microphone to record from; empty follows the
     * system default. A UID rather than a name or an index because both of
     * those move — see the platform_audio_input_devices comment. */
    char input_device[128];

    /* Hard ceiling on one utterance. Recording stops itself here so a
     * forgotten toggle cannot grow an unbounded buffer. */
    f32  max_seconds;

    /* Stop after this much trailing quiet once speech has been heard; 0
     * disables it and the user stops recording by hand. The speech
     * precondition matters: without it, the pause before someone starts
     * talking ends the recording immediately. */
    f32  silence_stop_sec;

    /* Press Return after the transcript is typed in. Off by default — a
     * misheard word is much cheaper to fix before it is submitted. */
    bool auto_submit;

    /* Play the two sounds of a dictation — the microphone opening, and
     * closing (voice_sound.h). On by default: with the card in a corner of the
     * window and eyes on the prompt, the sound is what says "go ahead". */
    bool sounds;
} VoiceConfig;

VoiceConfig voice_config_default(void);

/* True when a usable model is present at cfg->model_path. Answers for the
 * LOCAL backend only — ask voice_ready() when the backend matters. */
bool voice_model_ready(const VoiceConfig *cfg);

/* Can a dictation be started with this config? Covers whichever backend is
 * selected (local model present / API provider configured) and fills `why`
 * with a user-facing reason when it returns false. */
bool voice_ready(const VoiceConfig *cfg, char *why, usize why_cap);

/* Begin a session: request the microphone, start capture, and kick off the
 * model load in the background. Returns false if voice is disabled, a
 * session is already running, the model is missing, or the microphone was
 * refused; voice_error() then carries a reason. */
bool voice_start(const VoiceConfig *cfg);

/* Stop capturing and transcribe what was recorded. No-op unless recording. */
void voice_stop(void);

/* Abandon the session, discarding audio and any transcript in flight. */
void voice_cancel(void);

/* Opaque "who started this" tag, set by the caller at voice_start() and
 * readable while the session lives. A voice session is process-wide but its
 * transcript lands in one window's focused pane, so the UI uses this to draw
 * the recording indicator in that window alone instead of every open one.
 * Meaningless to this module — it is stored and handed back, nothing else. */
void  voice_set_owner(void *owner);
void *voice_owner(void);

/* The caller is playing a sound of its own into the room right now — the
 * "listening" cue. For the next `seconds` of THIS recording, what the
 * microphone hears cannot arm the stop-on-silence rule: a speaker heard by
 * the microphone beside it is loud enough to count as speech, and then the
 * pause before the user says anything (which the rule exists to sit through)
 * ends the recording. The audio itself is kept — a word spoken straight away
 * is not lost, and it arms the rule as soon as the window has passed. No-op
 * unless a recording is running. */
void  voice_guard_cue(f64 seconds);

/* Pump: drain the capture ring, apply the auto-stop rules, collect the
 * worker's result. Must be called once per frame while a session is live. */
void voice_tick(void);

VoiceState  voice_state(void);

/* Peak amplitude, 0..1, of the audio drained by the most recent voice_tick().
 *
 * The raw thing, not a meter: the platform layer's own level is smoothed with
 * an instant attack and a ~0.5 s release, which flattens a waveform drawn from
 * it into a slab. Whatever draws it does its own smoothing and owns the time
 * constants. */
f32         voice_level(void);

/* Peak amplitude since the last call, then reset to zero.
 *
 * A display that samples on a clock — the waveform pushes one bar every 55 ms
 * — must not sample voice_level() at those instants: it would read the peak of
 * whichever ~16 ms frame happened to land there and miss the rest of the
 * interval, which is a picture of the frame timing rather than of the audio.
 * This accumulates across the whole interval, so every sample the user has
 * spoken is represented in exactly one bar. */
/* The card's waveform, one bar at a time.
 *
 * A bar is the peak of VOICE_WAVE_BAR_MS of audio — of THOSE samples, cut out
 * of the stream by counting them. The microphone does not deliver by the
 * frame: the capture tap hands over 4096 frames at a time, ~85 ms, so a bar
 * taken as "the peak of whatever arrived since the last frame asked" would
 * get a whole buffer two times in three and nothing the third, leaving holes
 * in the trace. Counting samples makes each bar the same length of sound
 * whenever its samples happen to turn up; the card then shows them on its own
 * clock (ui/voice_hud_math.h).
 *
 * Copies up to `max` raw peaks (0..1, oldest first) heard since the last call
 * and returns how many; 0 unless a recording is running. */
#define VOICE_WAVE_BAR_MS 36
i32         voice_wave_take(f32 *out, i32 max);

f64         voice_elapsed(void);      /* seconds recorded so far            */
f32         voice_progress(void);     /* 0..1 through transcription         */
const char *voice_error(void);        /* "" when there is none              */
const char *voice_backend(void);      /* "metal"/"cpu"/provider, once known */
const char *voice_language(void);     /* detected or requested ISO code     */

/* Take the finished transcript. Returns NULL unless the state is
 * VOICE_DONE; the pointer stays valid until the next voice_start(), and the
 * session returns to VOICE_IDLE. */
const char *voice_take_text(void);

/* Join the worker thread; idempotent, call once on shutdown. */
void voice_shutdown(void);

#endif /* VOICE_VOICE_H */
