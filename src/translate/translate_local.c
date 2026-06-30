/*
 * Liu - Translate-on-Tab local-LLM backend.
 *
 * Threading: one persistent worker thread, lazily created on first
 * submit. main -> worker handoff is a mutex + condvar; worker -> main is
 * a mutex-guarded accumulation buffer plus a `done` flag (the plan's
 * "SPSC token ring drained every frame" — since the contract is "hand
 * over the whole text on EOS", a guarded buffer is the same contract
 * with less machinery). The LlmEngine is loaded on demand at the start of
 * each translation and freed the instant it finishes, so the model's
 * GPU-resident weights never linger between runs.
 *
 * Compiled only when USE_LOCAL_LLM=ON.
 */
#include "translate/translate_local.h"
#include "llm/llm_engine.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define TL_PROMPT_CAP 8192
#define TL_OUT_CAP    65536
#define TL_PATH_CAP    512
#define TL_MAX_TOKENS  256

static struct {
    pthread_mutex_t mtx;
    pthread_cond_t  cv;
    pthread_t       thread;
    bool            thread_started;

    /* main -> worker */
    bool            has_work;
    bool            shutdown;
    bool            cancel;      /* abort the in-flight generation early */
    char            prompt[TL_PROMPT_CAP];
    char            path[TL_PATH_CAP];

    /* worker -> main (guarded by mtx) */
    bool            started_notice;
    bool            running;
    bool            done;
    bool            ok;
    char            out[TL_OUT_CAP];
    i32             out_len;
    i32             drained;     /* bytes of `out` the main thread has streamed */
    bool            seen_nonws;  /* leading-whitespace skip, per generation */
} g_tl = {
    .mtx = PTHREAD_MUTEX_INITIALIZER,
    .cv  = PTHREAD_COND_INITIALIZER,
};

/* Streaming callback (worker thread) — append decoded bytes to the shared
 * buffer for the main thread to drain live. Leading whitespace is skipped
 * here (incrementally) since the buffer can no longer be trimmed in place
 * once the main thread has started streaming from it. Returns false to
 * abort generation on shutdown. */
static bool tl_on_token(void *user, const char *utf8, i32 len) {
    (void)user;
    pthread_mutex_lock(&g_tl.mtx);
    bool stop = g_tl.shutdown || g_tl.cancel;
    if (!stop && len > 0) {
        const char *p = utf8;
        i32 l = len;
        if (!g_tl.seen_nonws) {
            while (l > 0 && (unsigned char)*p <= ' ') { p++; l--; }
            if (l > 0) g_tl.seen_nonws = true;
        }
        if (l > 0 && g_tl.out_len + l < TL_OUT_CAP) {
            memcpy(g_tl.out + g_tl.out_len, p, (usize)l);
            g_tl.out_len += l;
            g_tl.out[g_tl.out_len] = '\0';
        }
    }
    pthread_mutex_unlock(&g_tl.mtx);
    return !stop;
}

static void *tl_worker(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_tl.mtx);
        while (!g_tl.has_work && !g_tl.shutdown) {
            pthread_cond_wait(&g_tl.cv, &g_tl.mtx);
        }
        if (g_tl.shutdown) {
            pthread_mutex_unlock(&g_tl.mtx);
            break;
        }
        char prompt[TL_PROMPT_CAP];
        char path[TL_PATH_CAP];
        memcpy(prompt, g_tl.prompt, sizeof prompt);
        memcpy(path, g_tl.path, sizeof path);
        g_tl.has_work       = false;
        g_tl.cancel         = false;
        g_tl.started_notice = true;
        g_tl.running        = true;
        g_tl.done           = false;
        g_tl.ok             = false;
        g_tl.out_len        = 0;
        g_tl.out[0]         = '\0';
        g_tl.drained        = 0;
        g_tl.seen_nonws     = false;
        pthread_mutex_unlock(&g_tl.mtx);

        /* Load the model for this translation. The engine is a worker-local
         * that lives only for this one generation — it is freed the instant
         * generation ends (below), so the ~0.5 GB of GPU-resident weights
         * never stay loaded between translations. Each run pays the GGUF
         * mmap + tokenizer-build + GPU-upload cost in exchange for keeping
         * nothing resident while idle. */
        char err[160];
        LlmEngine *engine = llm_engine_load(path, err, sizeof err);

        bool ok = false;
        if (engine) {
            i32 n = llm_engine_generate(engine, prompt, TL_MAX_TOKENS,
                                        tl_on_token, NULL);
            ok = (n > 0);
        }

        /* Publish the result, then tear the model down. The decoded text is
         * already in g_tl.out (independent of the engine), so freeing here
         * loses nothing. running/done flip under the lock; the slow GPU
         * teardown runs *outside* the lock so the main thread's per-frame
         * drain is never blocked on it. The engine is a worker-local, so no
         * other thread can ever observe it. */
        pthread_mutex_lock(&g_tl.mtx);
        g_tl.ok      = ok && g_tl.out_len > 0;
        g_tl.running = false;
        g_tl.done    = true;
        pthread_mutex_unlock(&g_tl.mtx);
        if (engine) llm_engine_free(engine);
    }
    return NULL;
}

bool translate_local_submit(const TranslateConfig *cfg, const char *text) {
    if (!cfg || !text) return false;
    if (!cfg->local_model_path[0]) return false;

    pthread_mutex_lock(&g_tl.mtx);
    if (g_tl.running || g_tl.has_work) {
        pthread_mutex_unlock(&g_tl.mtx);
        return false;                 /* one translation at a time */
    }
    translate_build_prompt(cfg, text, g_tl.prompt, sizeof g_tl.prompt);
    snprintf(g_tl.path, sizeof g_tl.path, "%s", cfg->local_model_path);
    g_tl.has_work = true;
    g_tl.done     = false;
    /* Reset the drain cursors now, under the lock. The worker also resets
     * these when it wakes, but a previous generation that was abandoned on
     * timeout can leave out_len > drained; without this, the first
     * translate_local_drain() of THIS request — which may run before the
     * worker wakes — would stream the stale leftover bytes into the prompt.
     * (We only reach here with running==false, so no concurrent writer.) */
    g_tl.out_len        = 0;
    g_tl.out[0]         = '\0';
    g_tl.drained        = 0;
    g_tl.seen_nonws     = false;
    g_tl.started_notice = false;
    bool need_thread = !g_tl.thread_started;
    pthread_mutex_unlock(&g_tl.mtx);

    if (need_thread) {
        if (pthread_create(&g_tl.thread, NULL, tl_worker, NULL) != 0) {
            pthread_mutex_lock(&g_tl.mtx);
            g_tl.has_work = false;
            pthread_mutex_unlock(&g_tl.mtx);
            return false;
        }
        g_tl.thread_started = true;
    }
    pthread_cond_signal(&g_tl.cv);
    return true;
}

i32 translate_local_drain(char *out, i32 cap, bool *done, bool *ok) {
    if (done) *done = false;
    if (ok) *ok = false;
    if (!out || cap <= 1) return 0;
    pthread_mutex_lock(&g_tl.mtx);
    i32 avail = g_tl.out_len - g_tl.drained;
    if (avail < 0) avail = 0;
    i32 n = avail < cap - 1 ? avail : cap - 1;
    if (n > 0) {
        memcpy(out, g_tl.out + g_tl.drained, (usize)n);
        g_tl.drained += n;
    }
    out[n] = '\0';
    /* Report `done` only once everything generated has been drained, so
     * the caller never misses a trailing token. */
    bool finished = g_tl.done && g_tl.drained >= g_tl.out_len;
    if (ok) *ok = g_tl.ok;
    if (done) *done = finished;
    if (finished) g_tl.done = false;   /* consume */
    pthread_mutex_unlock(&g_tl.mtx);
    return n;
}

bool translate_local_consume_started(void) {
    pthread_mutex_lock(&g_tl.mtx);
    bool started = g_tl.started_notice;
    g_tl.started_notice = false;
    pthread_mutex_unlock(&g_tl.mtx);
    return started;
}

void translate_local_cancel(void) {
    pthread_mutex_lock(&g_tl.mtx);
    /* Only meaningful while a generation is in flight; tl_on_token checks
     * this flag and returns false to stop generation promptly. The worker
     * clears it when it next picks up work. */
    if (g_tl.running || g_tl.has_work) g_tl.cancel = true;
    pthread_mutex_unlock(&g_tl.mtx);
}

bool translate_local_active(void) {
    pthread_mutex_lock(&g_tl.mtx);
    bool active = g_tl.running || g_tl.has_work;
    pthread_mutex_unlock(&g_tl.mtx);
    return active;
}

void translate_local_shutdown(void) {
    if (!g_tl.thread_started) return;
    pthread_mutex_lock(&g_tl.mtx);
    g_tl.shutdown = true;
    pthread_cond_signal(&g_tl.cv);
    pthread_mutex_unlock(&g_tl.mtx);
    pthread_join(g_tl.thread, NULL);
    g_tl.thread_started = false;
    /* No cached engine to release: the worker frees its (worker-local)
     * engine before it exits, including when shutdown aborts a generation
     * mid-flight (tl_on_token returns false on g_tl.shutdown). */
}
