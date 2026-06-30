/*
 * Liu - Translate-on-Tab: local-LLM backend (B izi).
 *
 * Declarations are always visible; the implementation
 * (translate_local.c) is compiled only when USE_LOCAL_LLM=ON, which also
 * defines LIU_HAVE_LOCAL_LLM. Call sites guard with that macro.
 *
 * One persistent worker thread owns the LlmEngine. translate_local_submit
 * hands it a prompt; the worker streams tokens into an internal buffer;
 * the main thread drains them live, token by token, via
 * translate_local_drain — which is what makes the on-Tab write stream.
 */
#ifndef TRANSLATE_TRANSLATE_LOCAL_H
#define TRANSLATE_TRANSLATE_LOCAL_H

#include "core/types.h"
#include "translate/translate.h"

/* Queue a translation on the worker thread. Returns false if a
 * translation is already in flight, no local model path is configured,
 * or the worker thread could not be started. */
bool translate_local_submit(const TranslateConfig *cfg, const char *text);

/* Main-thread streaming drain. Copies the tokens generated since the last
 * call into `out` (up to cap-1 bytes, always NUL-terminated) and returns
 * the byte count. *done is set true exactly once, after the final bytes
 * have been drained (worker hit EOS); *ok carries the success flag. The
 * caller writes the incremental bytes live and finishes up on *done. */
i32 translate_local_drain(char *out, i32 cap, bool *done, bool *ok);

/* True once when the worker has actually picked up a submitted generation.
 * The main thread uses this to surface a top-right "engine started" toast. */
bool translate_local_consume_started(void);

/* True while a submit is queued or a generation is running. */
bool translate_local_active(void);

/* Abort the in-flight generation (if any) at the next decoded token. The
 * worker thread stays alive and accepts the next submit; this just stops a
 * run the main thread has given up on (e.g. on timeout) so it doesn't keep
 * the backend busy decoding a result nobody will consume. Safe no-op when
 * nothing is in flight. */
void translate_local_cancel(void);

/* Join the worker thread. Idempotent; call once on app shutdown. The
 * engine is loaded and freed per-translation by the worker, so there is no
 * cached model left to release here. */
void translate_local_shutdown(void);

#endif /* TRANSLATE_TRANSLATE_LOCAL_H */
