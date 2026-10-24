/*
 * Liu - Voice input session.
 *
 * One worker thread, one mutex, one condition variable. The worker does two
 * things: load the model (started as soon as recording begins, so the setup
 * cost hides behind the user's sentence) and run the transcription (started
 * when they stop). The main thread only ever reads snapshots under the lock
 * and never blocks on the GPU.
 *
 * Audio is drained from the platform ring on the main thread rather than
 * handed to the worker sample by sample. The ring holds four seconds, a
 * frame is 16 ms, and pulling from it in voice_tick() keeps the audio
 * thread's only job "write into the ring" — no allocation, no locking, no
 * dependency on how busy the renderer is.
 */
#include "core/mem_budget.h"
#include "voice/voice.h"
#include "voice/voice_api.h"
#include "voice/voice_paths.h"
#include "voice/whisper_engine.h"
#include "voice/whisper_mel.h"
#include "platform/platform.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Anything quieter than this counts as silence for the auto-stop rule, and
 * anything louder arms it. Peak level, not RMS — the platform meter reports
 * peak, and a peak floor is far less sensitive to room tone than an RMS one. */
#define VOICE_SPEECH_LEVEL   0.045f

/* One bar of the card's waveform in samples, and how many may wait. */
#define VOICE_WAVE_WINDOW    (PLATFORM_CAPTURE_RATE * VOICE_WAVE_BAR_MS / 1000)
#define VOICE_WAVE_QUEUE     64

/* Transcripts of a dictated terminal prompt; generous, but bounded so a
 * repetition loop cannot grow without limit. */
#define VOICE_TEXT_CAP       (64 * 1024)

/* Below this much audio there is nothing worth sending to the model — a
 * mis-tapped shortcut rather than an utterance. */
#define VOICE_MIN_SAMPLES    (PLATFORM_CAPTURE_RATE / 4)

typedef enum { W_IDLE = 0, W_LOAD, W_RUN, W_QUIT } WorkerCmd;

typedef struct {
    u64 generation;
    VoiceConfig cfg;
    WhisperParams params;
    char model_path[512];
} VoiceRequest;

static struct {
    bool            started;
    pthread_t       tid;
    pthread_mutex_t mu;
    pthread_cond_t  cv;

    /* --- shared, guarded by mu --- */
    WorkerCmd       cmd;
    u64             generation;
    VoiceRequest    command_request, job_request;
    bool            engine_ready;
    bool            cancel;         /* worker drops its result and unloads   */
    char            model_path[512];
    WhisperParams   params;
    /* Snapshot of the session's config, taken at voice_start(). The worker
     * needs the backend and — for API — the provider block, and reading them
     * from the caller's VoiceConfig would be a race against Settings. */
    VoiceConfig     cfg;

    f32            *job;            /* samples handed over for one run       */
    i32             job_len;
    char            text[VOICE_TEXT_CAP];
    i32             text_len;
    bool            result_ready;
    bool            result_ok;
    char            err[192];
    char            backend[16];
    char            language[8];
    f32             progress;

    /* --- main thread only --- */
    VoiceState      state;
    f32            *pcm;
    i32             pcm_len, pcm_cap;
    i32             max_samples;
    f32             silence_stop_sec;
    bool            heard_speech;
    f64             silence_since;   /* seconds of recording at last loud frame */
    f64             guard_until;     /* seconds of recording: nothing arms before this */
    f64             started_at;
    f32             level;          /* peak of the audio this tick drained   */
    /* The card's bars: per-VOICE_WAVE_BAR_MS peaks waiting to be collected
     * (voice_wave_take), and the bar being counted out of the stream. */
    f32             wq[VOICE_WAVE_QUEUE];
    i32             wq_n;
    f32             wq_peak;
    i32             wq_fill;
    bool            overflowed;
    void           *owner;
} g;

/* Voice memory accounting includes the capture ring and the worker's job
 * buffer. The ring grows to VOICE_MAX_SEC float samples. Use transient_adjust
 * because the worker frees g.job on its own thread and mem_budget_set is
 * main-thread-only. */
static void voice_charge(isize bytes) {
    if (bytes) mem_budget_transient_adjust(MEM_VOICE, bytes);
}

/* ---- helpers ------------------------------------------------------------ */

static f64 voice_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (f64)ts.tv_sec + (f64)ts.tv_nsec / 1e9;
}

/* The lock is conditional because the one caller that can run before the
 * worker exists is the "could not start the worker" path itself — and by then
 * voice_worker_ensure() has already destroyed the mutex it failed to build a
 * thread around. */
static void voice_set_error(const char *msg) {
    if (g.started) pthread_mutex_lock(&g.mu);
    snprintf(g.err, sizeof g.err, "%s", msg ? msg : "");
    if (g.started) pthread_mutex_unlock(&g.mu);
    g.state = VOICE_ERROR;
}

VoiceConfig voice_config_default(void) {
    VoiceConfig c;
    memset(&c, 0, sizeof c);
    c.enabled = true;
    /* Local by default: dictation then costs nothing and sends nothing off
     * the machine, and uploading the microphone's audio is an explicit
     * opt-in. */
    c.backend = VOICE_BACKEND_LOCAL;
    liu_whisper_default_path(c.model_path, sizeof c.model_path);
    c.lang[0]          = '\0';    /* auto-detect */
    c.translate        = false;
    c.max_seconds      = 120.0f;
    c.silence_stop_sec = 2.0f;
    c.auto_submit      = false;
    c.sounds           = true;
    snprintf(c.api_provider, sizeof c.api_provider, "openai");
    return c;
}

bool voice_model_ready(const VoiceConfig *cfg) {
    if (!cfg) return false;
    char fallback[512];
    const char *p = cfg->model_path[0] ? cfg->model_path
                                       : liu_whisper_default_path(fallback, sizeof fallback);
    return liu_whisper_file_ok(p);
}

bool voice_ready(const VoiceConfig *cfg, char *why, usize why_cap) {
    if (why && why_cap) why[0] = '\0';
    if (!cfg) return false;
    if (cfg->backend == VOICE_BACKEND_API)
        return voice_api_ready(cfg, why, why_cap);
    if (!voice_model_ready(cfg)) {
        if (why && why_cap) snprintf(why, why_cap, "the speech model is not downloaded");
        return false;
    }
    return true;
}

/* ---- worker ------------------------------------------------------------- */

static bool voice_worker_progress(void *user, f32 fraction) {
    const VoiceRequest *request = user;
    pthread_mutex_lock(&g.mu);
    const bool keep = request && request->generation == g.generation && !g.cancel;
    if (keep) g.progress = fraction;
    pthread_mutex_unlock(&g.mu);
    return keep;
}

/* Cancellation probe for the API backend. There is no fraction to report — an
 * HTTP transcription is one opaque round trip — so the card shows its
 * indeterminate shuttle and `progress` stays 0. */
static bool voice_worker_keep_going(void *user) {
    const VoiceRequest *request = user;
    pthread_mutex_lock(&g.mu);
    const bool keep = request && request->generation == g.generation && !g.cancel;
    pthread_mutex_unlock(&g.mu);
    return keep;
}

/* Caller holds g.mu. Each accepted command owns values, not the next session's
 * mutable config. A request remains valid through its synchronous callbacks. */
static VoiceRequest voice_request_locked(void) {
    VoiceRequest request = { .generation = g.generation, .cfg = g.cfg, .params = g.params };
    snprintf(request.model_path, sizeof request.model_path, "%s", g.model_path);
    return request;
}

static bool voice_request_current_locked(const VoiceRequest *request) {
    return request->generation == g.generation && !g.cancel;
}

static WhisperEngine *voice_load_current(const VoiceRequest *request,
                                         char *err, usize err_cap) {
    if (!voice_worker_keep_going((void *)request)) return NULL;
    WhisperEngine *engine = whisper_engine_load(request->model_path, err, err_cap);
    if (!voice_worker_keep_going((void *)request)) {
        whisper_engine_free(engine);
        return NULL;
    }
    return engine;
}

static void *voice_worker(void *arg) {
    (void)arg;
    WhisperEngine *engine = NULL;
    u64 engine_generation = 0;

    for (;;) {
        pthread_mutex_lock(&g.mu);
        while (g.cmd == W_IDLE) pthread_cond_wait(&g.cv, &g.mu);
        WorkerCmd cmd = g.cmd;
        g.cmd = W_IDLE;
        if (cmd == W_QUIT) {
            pthread_mutex_unlock(&g.mu);
            break;
        }
        VoiceRequest request = g.command_request;
        f32 *job = NULL;
        i32 n = 0;
        if (cmd == W_RUN && g.job && g.job_request.generation == request.generation) {
            job = g.job; n = g.job_len;
            g.job = NULL; g.job_len = 0;
        }
        pthread_mutex_unlock(&g.mu);

        char err[192] = { 0 };
        if (cmd == W_LOAD) {
            whisper_engine_free(engine); engine = NULL; engine_generation = 0;
            if (request.cfg.backend == VOICE_BACKEND_LOCAL) {
                engine = voice_load_current(&request, err, sizeof err);
                if (engine) engine_generation = request.generation;
            }
            pthread_mutex_lock(&g.mu);
            if (voice_request_current_locked(&request)) {
                g.engine_ready = request.cfg.backend == VOICE_BACKEND_API || engine != NULL;
                if (g.engine_ready) {
                    snprintf(g.backend, sizeof g.backend, "%s",
                        request.cfg.backend == VOICE_BACKEND_API
                        ? voice_api_provider_display(request.cfg.api_provider)
                        : whisper_engine_backend(engine));
                } else snprintf(g.err, sizeof g.err, "%s", err[0] ? err : "model load failed");
            }
            pthread_mutex_unlock(&g.mu);
            continue;
        }

        /* The worker owns both the sample buffer and its immutable request.
         * A quick stop can replace a queued preload: load the exact run's
         * model here if no matching preload reached this worker yet. */
        if (engine && (engine_generation != request.generation ||
                       request.cfg.backend == VOICE_BACKEND_API)) {
            whisper_engine_free(engine); engine = NULL; engine_generation = 0;
        }
        bool current = voice_worker_keep_going(&request);
        if (current && job && n > 0 && request.cfg.backend == VOICE_BACKEND_LOCAL && !engine) {
            engine = voice_load_current(&request, err, sizeof err);
            if (engine) {
                engine_generation = request.generation;
                pthread_mutex_lock(&g.mu);
                if (voice_request_current_locked(&request)) {
                    g.engine_ready = true;
                    snprintf(g.backend, sizeof g.backend, "%s", whisper_engine_backend(engine));
                }
                pthread_mutex_unlock(&g.mu);
            }
        }
        current = voice_worker_keep_going(&request);
        static char text[VOICE_TEXT_CAP];
        char language[8] = { 0 };
        i32 len = -1;
        if (current && job && n > 0) {
            if (request.cfg.backend == VOICE_BACKEND_API) {
                len = voice_api_transcribe(&request.cfg, job, n,
                    text, (i32)sizeof text, language, sizeof language, err, sizeof err,
                    voice_worker_keep_going, &request);
            } else if (engine) {
                whisper_engine_set_progress(engine, voice_worker_progress, &request);
                len = whisper_engine_transcribe(engine, job, n, &request.params,
                                                text, (i32)sizeof text);
                snprintf(language, sizeof language, "%s", whisper_engine_language(engine));
            }
        }
        bool had_samples = job && n > 0;
        voice_charge(-(isize)n * (isize)sizeof *job); free(job);
        pthread_mutex_lock(&g.mu);
        if (voice_request_current_locked(&request)) {
            g.result_ok = len >= 0;
            if (len >= 0) {
                g.text_len = len;
                memcpy(g.text, text, (usize)len);
                g.text[len] = '\0';
                snprintf(g.language, sizeof g.language, "%s", language);
            } else if (!g.err[0]) {
                snprintf(g.err, sizeof g.err, "%s", err[0] ? err
                         : !had_samples ? "nothing was recorded" : "transcription failed");
            }
            g.result_ready = true;
        }
        pthread_mutex_unlock(&g.mu);

        /* Release the model outside the mailbox lock so teardown for one
         * session cannot change readiness for a session accepted during
         * cleanup. */
        whisper_engine_free(engine); engine = NULL; engine_generation = 0;
        pthread_mutex_lock(&g.mu);
        if (request.generation == g.generation) g.engine_ready = false;
        pthread_mutex_unlock(&g.mu);
    }
    whisper_engine_free(engine);
    return NULL;
}

static bool voice_worker_ensure(void) {
    if (g.started) return true;
    if (pthread_mutex_init(&g.mu, NULL) != 0) return false;
    if (pthread_cond_init(&g.cv, NULL) != 0) {
        pthread_mutex_destroy(&g.mu);
        return false;
    }
    if (pthread_create(&g.tid, NULL, voice_worker, NULL) != 0) {
        pthread_cond_destroy(&g.cv);
        pthread_mutex_destroy(&g.mu);
        return false;
    }
    g.started = true;
    return true;
}

static void voice_post(WorkerCmd cmd) {
    pthread_mutex_lock(&g.mu);
    g.command_request = voice_request_locked();
    g.cmd = cmd;
    pthread_cond_signal(&g.cv);
    pthread_mutex_unlock(&g.mu);
}

/* ---- session ------------------------------------------------------------ */

bool voice_start(const VoiceConfig *cfg) {
    if (!cfg || !cfg->enabled) return false;
    if (g.state != VOICE_IDLE && g.state != VOICE_ERROR && g.state != VOICE_DONE) return false;

    if (!voice_worker_ensure()) { voice_set_error("cannot start the speech worker"); return false; }

    char fallback[512];
    const char *model = cfg->model_path[0] ? cfg->model_path
                                           : liu_whisper_default_path(fallback, sizeof fallback);
    /* liu_whisper_default_path returns NULL without $HOME. The API backend
     * passes the file check below without ever looking at a path, so the
     * NULL has to be handled here. */
    if (!model) model = "";
    char why[192];
    if (!voice_ready(cfg, why, sizeof why)) {
        voice_set_error(why[0] ? why : "voice input is not configured");
        return false;
    }

    pthread_mutex_lock(&g.mu);
    g.generation++;
    g.engine_ready = false;
    g.backend[0] = '\0';
    g.cfg          = *cfg;
    g.err[0]       = '\0';
    g.text[0]      = '\0';
    g.text_len     = 0;
    g.result_ready = false;
    g.result_ok    = false;
    g.cancel       = false;
    g.progress     = 0.0f;
    g.language[0]  = '\0';
    snprintf(g.model_path, sizeof g.model_path, "%s", model);
    memset(&g.params, 0, sizeof g.params);
    snprintf(g.params.lang, sizeof g.params.lang, "%s", cfg->lang);
    g.params.translate = cfg->translate;
    pthread_mutex_unlock(&g.mu);

    f32 secs = cfg->max_seconds > 1.0f ? cfg->max_seconds : 120.0f;
    g.max_samples      = (i32)(secs * PLATFORM_CAPTURE_RATE);
    g.silence_stop_sec = cfg->silence_stop_sec;
    g.pcm_len          = 0;
    g.heard_speech     = false;
    g.silence_since    = 0.0;
    g.guard_until      = 0.0;
    g.level            = 0.0f;
    g.wq_n = 0; g.wq_peak = 0.0f; g.wq_fill = 0;
    g.overflowed       = false;

    if (g.pcm_cap < g.max_samples) {
        f32 *p = realloc(g.pcm, (usize)g.max_samples * sizeof *p);
        if (!p) { voice_set_error("out of memory"); return false; }
        voice_charge(((isize)g.max_samples - (isize)g.pcm_cap) * (isize)sizeof *p);
        g.pcm = p;
        g.pcm_cap = g.max_samples;
    }

    /* Pin the microphone before opening it — the device is bound when the
     * capture engine starts and cannot be swapped under a running one. */
    platform_audio_set_input_device(cfg->input_device);

    if (!platform_audio_capture_start()) {
        voice_set_error(platform_audio_mic_permission() == PLATFORM_MIC_DENIED
                        ? "microphone access is off for Liu"
                        : "cannot open the microphone");
        return false;
    }

    /* Load while the user talks. */
    voice_post(W_LOAD);


    platform_audio_capture_overflowed();   /* clear a stale flag */
    g.started_at = voice_now();
    g.state = platform_audio_capturing() ? VOICE_RECORDING : VOICE_WAITING_MIC;
    return true;
}

/* After a recording ends, shrink the capture buffer only when its capacity
 * exceeds twice the 120-second retention target. This avoids reallocating for
 * sessions up to the threshold while releasing capacity left by much longer
 * recordings. Called from main-thread session-end paths; allocation and free
 * do not occur on the audio callback. */
#define VOICE_PCM_RETAIN_SEC 120.0f



static void voice_pcm_release_high_water(void) {
    i32 retain = (i32)(VOICE_PCM_RETAIN_SEC * (f32)PLATFORM_CAPTURE_RATE);
    if (g.pcm_cap <= retain * 2) return;      /* inside the hysteresis band */
    f32 *p = realloc(g.pcm, (usize)retain * sizeof *p);
    if (!p) return;                            /* keep the big one; harmless */
    voice_charge(((isize)retain - (isize)g.pcm_cap) * (isize)sizeof *p);
    g.pcm     = p;
    g.pcm_cap = retain;
    g.pcm_len = 0;
}

/* Hand the recorded audio to the worker. */
static void voice_submit(void) {
    platform_audio_capture_stop();

    if (g.pcm_len < VOICE_MIN_SAMPLES) {
        voice_set_error("nothing was recorded");
        pthread_mutex_lock(&g.mu);
        g.cancel = true;                  /* let the worker drop the model */
        pthread_mutex_unlock(&g.mu);
        voice_post(W_RUN);
        voice_pcm_release_high_water();
        return;
    }

    f32 *copy = malloc((usize)g.pcm_len * sizeof *copy);
    if (!copy) { voice_set_error("out of memory"); return; }
    memcpy(copy, g.pcm, (usize)g.pcm_len * sizeof *copy);

    pthread_mutex_lock(&g.mu);
    voice_charge(-(isize)g.job_len * (isize)sizeof *copy);
    free(g.job);
    voice_charge((isize)g.pcm_len * (isize)sizeof *copy);
    g.job     = copy;
    g.job_len = g.pcm_len;
    g.job_request = voice_request_locked();
    g.command_request = g.job_request;
    g.cmd = W_RUN;
    pthread_cond_signal(&g.cv);
    pthread_mutex_unlock(&g.mu);

    g.state = VOICE_TRANSCRIBING;
    /* The samples are in `copy` now; the capture buffer is dead until the next
     * start, which is the moment to hand back an unusually large one. */
    voice_pcm_release_high_water();
}

void voice_stop(void) {
    if (g.state == VOICE_WAITING_MIC) { voice_cancel(); return; }
    if (g.state != VOICE_RECORDING) return;
    voice_tick();          /* pick up the last few milliseconds */
    /* The tick may have submitted on its own (silence or length rule), which
     * would make this a second submit of a buffer already handed over. */
    if (g.state == VOICE_RECORDING) voice_submit();
}

void voice_set_owner(void *owner) { g.owner = owner; }
void *voice_owner(void) { return g.owner; }

void voice_guard_cue(f64 seconds) {
    if (g.state != VOICE_RECORDING || !(seconds > 0.0)) return;
    g.guard_until = (f64)g.pcm_len / (f64)PLATFORM_CAPTURE_RATE + seconds;
}

void voice_cancel(void) {
    platform_audio_capture_stop();
    if (g.started) {
        pthread_mutex_lock(&g.mu);
        g.generation++;
        g.engine_ready = false;
        g.cancel       = true;
        g.result_ready = false;
        voice_charge(-(isize)g.job_len * (isize)sizeof *g.job);
        free(g.job);
        g.job     = NULL;
        g.job_len = 0;
        pthread_mutex_unlock(&g.mu);
        /* A load may be in flight; W_RUN makes the worker notice the cancel
         * and release the engine instead of leaving 600 MB resident. */
        voice_post(W_RUN);
    }
    g.pcm_len   = 0;
    g.level     = 0.0f;
    g.wq_n = 0; g.wq_peak = 0.0f; g.wq_fill = 0;
    g.state     = VOICE_IDLE;
    voice_pcm_release_high_water();
}

void voice_tick(void) {
    if (g.state == VOICE_IDLE) return;

    if (g.state == VOICE_WAITING_MIC) {
        if (platform_audio_capturing()) {
            g.state = VOICE_RECORDING;
            g.started_at = voice_now();
        } else if (platform_audio_mic_permission() == PLATFORM_MIC_DENIED) {
            voice_cancel();
            voice_set_error("microphone access is off for Liu");
        }
        return;
    }

    if (g.state == VOICE_RECORDING) {
        i32 room = g.pcm_cap - g.pcm_len;
        if (room > 0) {
            f32 *dst = g.pcm + g.pcm_len;
            i32 got = platform_audio_capture_read(dst, room);
            /* Measure the raw peak from newly captured samples. The platform
             * meter already applies about a 0.5-second release, which smooths
             * away syllable-level changes when a waveform applies its own
             * smoothing. Both consumers use the raw value and shape it
             * independently. */
            if (got > 0) {
                f32 peak = 0.0f;
                for (i32 i = 0; i < got; i++) {
                    f32 a = fabsf(dst[i]);
                    if (a > peak) peak = a;
                    /* ...and the card's bars, counted out of the same samples. */
                    if (a > g.wq_peak) g.wq_peak = a;
                    if (++g.wq_fill >= VOICE_WAVE_WINDOW) {
                        if (g.wq_n == VOICE_WAVE_QUEUE) {     /* nobody is drawing: keep the newest */
                            memmove(g.wq, g.wq + 1, (VOICE_WAVE_QUEUE - 1) * sizeof g.wq[0]);
                            g.wq_n--;
                        }
                        g.wq[g.wq_n++] = g.wq_peak;
                        g.wq_peak = 0.0f; g.wq_fill = 0;
                    }
                }
                g.level = peak;
            }
            /* got == 0 holds the previous level on purpose: a frame shorter
             * than the capture callback's hop is not silence. */
            g.pcm_len += got;
        }
        if (platform_audio_capture_overflowed()) g.overflowed = true;

        const f64 elapsed = (f64)g.pcm_len / (f64)PLATFORM_CAPTURE_RATE;

        /* Not while the app's own cue is in the room (voice_guard_cue). */
        if (g.level >= VOICE_SPEECH_LEVEL && elapsed >= g.guard_until) {
            g.heard_speech  = true;
            g.silence_since = elapsed;
        }

        if (g.pcm_len >= g.max_samples) { voice_submit(); return; }

        if (g.silence_stop_sec > 0.0f && g.heard_speech &&
            elapsed - g.silence_since >= (f64)g.silence_stop_sec) {
            voice_submit();
            return;
        }
        return;
    }

    if (g.state == VOICE_TRANSCRIBING) {
        pthread_mutex_lock(&g.mu);
        const bool ready = g.result_ready;
        const bool ok    = g.result_ok;
        if (ready) g.result_ready = false;
        pthread_mutex_unlock(&g.mu);

        if (ready) g.state = ok ? VOICE_DONE : VOICE_ERROR;
    }
}

VoiceState voice_state(void) { return g.state; }

f32 voice_level(void) { return g.state == VOICE_RECORDING ? g.level : 0.0f; }

i32 voice_wave_take(f32 *out, i32 max) {
    if (g.state != VOICE_RECORDING) { g.wq_n = 0; return 0; }
    if (!out || max <= 0) return 0;
    i32 n = g.wq_n < max ? g.wq_n : max;
    memcpy(out, g.wq, (usize)n * sizeof g.wq[0]);
    g.wq_n -= n;
    if (g.wq_n > 0) memmove(g.wq, g.wq + n, (usize)g.wq_n * sizeof g.wq[0]);
    return n;
}

f64 voice_elapsed(void) {
    if (g.state != VOICE_RECORDING && g.state != VOICE_TRANSCRIBING) return 0.0;
    return (f64)g.pcm_len / (f64)PLATFORM_CAPTURE_RATE;
}

f32 voice_progress(void) {
    if (g.state != VOICE_TRANSCRIBING || !g.started) return 0.0f;
    pthread_mutex_lock(&g.mu);
    f32 p = g.progress;
    pthread_mutex_unlock(&g.mu);
    return p;
}

const char *voice_error(void) {
    static char buf[192];
    if (!g.started) return g.err;   /* set before the worker existed */
    pthread_mutex_lock(&g.mu);
    snprintf(buf, sizeof buf, "%s", g.err);
    pthread_mutex_unlock(&g.mu);
    return buf;
}

const char *voice_backend(void) {
    static char buf[16];
    if (!g.started) return "";
    pthread_mutex_lock(&g.mu);
    snprintf(buf, sizeof buf, "%s", g.backend);
    pthread_mutex_unlock(&g.mu);
    return buf;
}

const char *voice_language(void) {
    static char buf[8];
    if (!g.started) return "";
    pthread_mutex_lock(&g.mu);
    snprintf(buf, sizeof buf, "%s", g.language);
    pthread_mutex_unlock(&g.mu);
    return buf;
}

const char *voice_take_text(void) {
    if (g.state != VOICE_DONE) return NULL;
    static char buf[VOICE_TEXT_CAP];
    pthread_mutex_lock(&g.mu);
    memcpy(buf, g.text, (usize)g.text_len + 1);
    pthread_mutex_unlock(&g.mu);
    g.state = VOICE_IDLE;
    return buf;
}

void voice_shutdown(void) {
    if (g.started) {
        platform_audio_capture_stop();
        pthread_mutex_lock(&g.mu);
        g.generation++;
        g.cancel = true;
        pthread_mutex_unlock(&g.mu);
        voice_post(W_QUIT);
        pthread_join(g.tid, NULL);
        pthread_cond_destroy(&g.cv);
        pthread_mutex_destroy(&g.mu);
        g.started = false;
    }
    voice_charge(-(isize)g.pcm_cap * (isize)sizeof *g.pcm);
    free(g.pcm);
    g.pcm = NULL;
    g.pcm_cap = g.pcm_len = 0;
    voice_charge(-(isize)g.job_len * (isize)sizeof *g.job);
    free(g.job);
    g.job = NULL;
    g.job_len = 0;
    g.state = VOICE_IDLE;
}
