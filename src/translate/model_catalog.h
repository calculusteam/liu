/*
 * Liu - Translate model catalog
 *
 * Async, cached model-id lists for the Translate tab's Model dropdown.
 * Two families of sources share one fetch pump:
 *
 *   Agent CLIs  — "claude" / "codex" / "opencode" come from one shared
 *                 models.dev/api.json download (anthropic, openai and
 *                 opencode-go providers respectively; opencode is
 *                 deliberately restricted to the OpenCode Go plan's
 *                 provider, ids stored as "opencode-go/<model>").
 *                 "grok" runs `grok models` and parses the bullet list.
 *
 *   API providers — "anthropic" / "openai" / "custom" GET their
 *                 /v1/models endpoint with the user's key (never on
 *                 argv — curl reads headers from stdin via `-K -`).
 *                 "openrouter"'s public list needs no auth.
 *
 * Fetches are fork+exec'd curl/grok children with non-blocking stdout
 * pipes, pumped by model_catalog_tick() from the main loop — the UI never
 * blocks. Agent lists persist to ~/.config/Liu/agent_models.json with a
 * 24 h TTL; API lists stay in memory (key-dependent) with a 1 h TTL.
 * While a list is IDLE/FETCHING/FAILED the UI falls back to its static
 * presets, so the dropdown is never empty.
 */
#ifndef TRANSLATE_MODEL_CATALOG_H
#define TRANSLATE_MODEL_CATALOG_H

#include "core/types.h"

#define MODEL_CATALOG_MAX 24
#define MODEL_ID_CAP      64

typedef enum {
    MODEL_LIST_IDLE = 0,
    MODEL_LIST_FETCHING,
    MODEL_LIST_READY,
    MODEL_LIST_FAILED,
} ModelListState;

typedef struct {
    char           ids[MODEL_CATALOG_MAX][MODEL_ID_CAP];
    i32            count;
    ModelListState state;
    /* time(NULL) of the last successful fetch; 0 = never. */
    long long      fetched_at;
} ModelList;

/* Pump in-flight fetch children (drain pipes, reap, parse). Cheap when
 * idle — call once per frame from the main loop. */
void model_catalog_tick(void);

/* SIGKILL + reap any in-flight fetch child and free its buffers. Call on
 * every app-exit path (atexit + app_destroy) so a fetch in flight at quit
 * — especially the timeout-less `grok models` child — is never orphaned. */
void model_catalog_shutdown(void);

/* Kick an async refresh for one agent CLI's list ("claude"|"codex"|
 * "opencode"|"grok"). No-op while fresh, already fetching, or within the
 * 60 s failure-retry window — safe to call every rendered frame. */
void model_catalog_request_agent(const char *agent_id);

/* Same, for an API provider ("anthropic"|"openai"|"openrouter"|"custom").
 * `api_key` must be the resolved key (config value or env fallback; may be
 * empty for openrouter/custom). `base_url` is only read for "custom".
 * A key/base change invalidates the cached list and refetches. */
void model_catalog_request_api(const char *provider, const char *api_key,
                               const char *base_url);

/* Read-only views; never NULL for known ids (NULL for unknown). The
 * returned struct lives in module storage and is updated in place by
 * model_catalog_tick — consume within the frame. */
const ModelList *model_catalog_agent(const char *agent_id);
const ModelList *model_catalog_api(const char *provider);

#endif /* TRANSLATE_MODEL_CATALOG_H */
