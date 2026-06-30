/*
 * Liu - Translate-on-Tab: API backend.
 *
 * fork+exec `curl -K -` against a hosted chat endpoint. The whole request
 * (URL, auth header, JSON body) rides curl's stdin config so the API key
 * never appears on argv — `ps` only ever shows "curl -K -". Reuses
 * TranslateAgentSpawn so main.c's existing child_pid/stdout_fd pump and
 * 30 s timeout apply unchanged; the response is buffered JSON, finalized
 * (not streamed) once the child exits.
 *
 * Providers: "anthropic" (/v1/messages), "openai" / "openrouter" /
 * "custom" (OpenAI-style /chat/completions; custom = any compatible
 * server: ollama, vllm, LM Studio, llama.cpp…).
 */
#ifndef TRANSLATE_TRANSLATE_API_H
#define TRANSLATE_TRANSLATE_API_H

#include "core/types.h"
#include "translate/translate.h"
#include "translate/translate_agent.h"

/* Effective API key: cfg->api_key if set, else the provider's
 * conventional env var (ANTHROPIC_API_KEY / OPENAI_API_KEY /
 * OPENROUTER_API_KEY). "custom" deliberately has no env fallback — a
 * stray OPENAI_API_KEY must never leak to a third-party base URL.
 * Returns "" when no key is available. */
const char *translate_api_effective_key(const TranslateConfig *cfg,
                                        char *buf, usize cap);

/* Can a request be attempted with this config? Fills `why` (for the
 * refusal toast) when returning false. */
bool translate_api_ready(const TranslateConfig *cfg,
                         char *why, usize why_cap);

/* Model used when cfg->api_model is empty. */
const char *translate_api_default_model(const char *provider);

/* "Anthropic API" / "OpenAI API" / … for status toasts. */
const char *translate_api_provider_display(const char *provider);

/* Build the request and fork+exec curl. Same contract as
 * translate_agent_spawn. */
bool translate_api_spawn(const TranslateConfig *cfg,
                         const char *text,
                         TranslateAgentSpawn *out);

/* Parse the buffered HTTP response body: extract the assistant text
 * (content[].text for anthropic, choices[0].message.content otherwise),
 * trim, copy into `out`. On failure returns false and puts a short
 * human-readable reason (API error message if present) into `err`. */
bool translate_api_finalize(const TranslateConfig *cfg,
                            const char *log, i32 log_len,
                            char *out, usize out_cap,
                            char *err, usize err_cap);

#endif /* TRANSLATE_TRANSLATE_API_H */
