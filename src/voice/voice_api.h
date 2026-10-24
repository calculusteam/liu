/*
 * Hosted speech-to-text backend for voice input.
 *
 * The backend posts recorded audio to a configured HTTP transcription endpoint
 * and runs on the voice worker. It passes curl configuration through stdin so
 * the API key does not appear in argv, polls keep_going while reading the
 * response, and terminates curl on cancellation.
 *
 * Supported providers are openai, groq, elevenlabs, and custom
 * OpenAI-compatible endpoints. The providers use different endpoints,
 * authorization headers, or form fields; custom has no provider-specific API
 * key fallback.
 */
#ifndef VOICE_VOICE_API_H
#define VOICE_VOICE_API_H

#include "core/types.h"
#include "voice/voice.h"

/* Effective API key: cfg->api_key when set, else the provider's conventional
 * env var. "custom" deliberately has no env fallback — a stray OPENAI_API_KEY
 * must never be sent to a third-party base URL. Returns "" when there is
 * none. */
const char *voice_api_effective_key(const VoiceConfig *cfg,
                                    char *buf, usize cap);

/* Can a request be attempted with this config? Fills `why` with a user-facing
 * reason when it returns false. */
bool voice_api_ready(const VoiceConfig *cfg, char *why, usize why_cap);

/* Model used when cfg->api_model is empty ("" for custom, which must pick). */
const char *voice_api_default_model(const char *provider);

/* "OpenAI" / "Groq" / … for the settings caption and status toasts. */
const char *voice_api_provider_display(const char *provider);

/* Transcribe 16 kHz mono f32 PCM through the configured endpoint. Blocking;
 * runs on the voice worker thread. Writes NUL-terminated UTF-8 into `out` and
 * returns its byte length, or -1 with a short reason in `err`.
 *
 * `lang_out` receives the language the service reported when it reports one,
 * otherwise the requested code. `keep_going` is polled roughly five times a
 * second while the request is in flight; returning false kills the transfer.
 * Both may be NULL. */
i32 voice_api_transcribe(const VoiceConfig *cfg,
                         const f32 *pcm, i32 n_samples,
                         char *out, i32 out_cap,
                         char *lang_out, usize lang_cap,
                         char *err, usize err_cap,
                         bool (*keep_going)(void *user), void *user);

#endif /* VOICE_VOICE_API_H */
