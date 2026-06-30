/*
 * Liu - Translate-on-Tab
 *
 * Foreground-agent-aware prompt-line translation. The user types a prompt in
 * their native language into a coding-agent CLI (claude/codex/opencode),
 * presses Ctrl+Cmd twice within `tab_window_sec`, and Liu replaces
 * the typed prompt text with the English (or `target_lang`) rendering. Chord
 * detection lives in main.c; this module is just config + prompt formatting.
 *
 * Three backends sit behind the same submit/tick pair:
 *   - AGENT — fork+exec one of the detected agent CLIs (claude -p PROMPT
 *             etc.) and capture stdout. Mirrors the Create-Theme pattern.
 *   - LOCAL — saf-C GGUF inference (USE_LOCAL_LLM=ON). Stubbed out at
 *             link time when the flag is off.
 *   - API   — fork+exec curl against a hosted chat-completions endpoint
 *             (Anthropic / OpenAI / OpenRouter / custom OpenAI-compatible).
 *             Same child_pid/stdout_fd plumbing as AGENT; the response is
 *             JSON so the tick finalizes via translate_api_finalize instead
 *             of streaming raw bytes.
 */
#ifndef TRANSLATE_TRANSLATE_H
#define TRANSLATE_TRANSLATE_H

#include "core/agent_detect.h"
#include "core/types.h"

typedef enum {
    TRANSLATE_BACKEND_AGENT = 0,
    TRANSLATE_BACKEND_LOCAL = 1,
    TRANSLATE_BACKEND_API   = 2,
} TranslateBackend;

typedef struct TranslateConfig {
    bool             enabled;
    TranslateBackend backend;
    char             agent_id[AGENT_ID_CAP];      /* "claude"|"codex"|"opencode" */
    char             agent_model[64];             /* optional; passed only if non-empty */
    char             local_model_path[512];       /* GGUF (B izi) */
    /* API backend — provider id is one of "anthropic"|"openai"|"openrouter"|
     * "custom". api_key may be empty: the spawn falls back to the provider's
     * conventional env var (ANTHROPIC_API_KEY / OPENAI_API_KEY / …).
     * api_base_url is only consulted for "custom" (an OpenAI-compatible
     * /chat/completions server: ollama, vllm, LM Studio, llama.cpp, …). */
    char             api_provider[16];
    char             api_model[64];
    char             api_key[256];
    char             api_base_url[256];
    char             source_lang[24];             /* default "Turkish" */
    char             target_lang[24];             /* default "English" */
    f32              tab_window_sec;              /* default 0.4 */
    bool             active_in_claude;
    bool             active_in_codex;
    bool             active_in_opencode;
    bool             active_in_grok;
} TranslateConfig;

TranslateConfig translate_config_default(void);

i32 translate_language_count(void);
const char *translate_language_name(i32 index);
void translate_cycle_language(char *lang, usize lang_cap);
void translate_normalize_direction(TranslateConfig *cfg);

/* Is `agent_id` (one of "claude"/"codex"/"opencode") gated on
 * in this config? Returns true for an empty or unknown id so callers can
 * shortcut the check at the call site and still get a sensible default. */
bool translate_active_in(const TranslateConfig *cfg, const char *agent_id);

/* Render the canonical translation prompt:
 *   "Translate the following segment into <target>, without additional
 *    explanation. <text>"
 * Truncates if `out_cap` is too small; always NUL-terminates. */
void translate_build_prompt(const TranslateConfig *cfg,
                            const char *text,
                            char *out, usize out_cap);

/* Forward — defined inline in translate.c's dispatcher path so callers
 * don't have to know the backend split. */
typedef void (*TranslateCompletionFn)(void *user, bool ok, const char *text);

#endif /* TRANSLATE_TRANSLATE_H */
