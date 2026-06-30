/*
 * Liu - Translate-on-Tab: config defaults + prompt builder.
 *
 * Backend dispatch (AGENT/LOCAL) is done at the call site in main.c —
 * the agent backend pokes child_pid/stdout_fd onto AppState and is
 * polled from app_tick_translate; the local backend (when USE_LOCAL_LLM
 * is on) pushes onto a SPSC ring drained by the same tick.
 */
#include "translate/translate.h"
#include "translate/model_paths.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

static const char *g_translate_languages[] = {
    "German",
    "Czech",
    "Danish",
    "Estonian",
    "Dutch",
    "Finnish",
    "French",
    "Spanish",
    "Swedish",
    "Italian",
    "Icelandic",
    "Polish",
    "Hungarian",
    "Norwegian",
    "Portuguese",
    "Romanian",
    "Russian",
    "Ukrainian",
    "Greek",
    "Simplified Chinese",
    "Bengali",
    "Indonesian",
    "Hindi",
    "Japanese",
    "Korean",
    "Malay",
    "Marathi",
    "Thai",
    "Urdu",
    "Vietnamese",
    "Arabic",
    "English",
    "Turkish",
};

TranslateConfig translate_config_default(void) {
    TranslateConfig c = {0};
    c.enabled = true;
    c.backend = TRANSLATE_BACKEND_LOCAL;
    snprintf(c.agent_id, sizeof(c.agent_id), "claude");
    c.agent_model[0] = '\0';
    c.local_model_path[0] = '\0';
    snprintf(c.api_provider, sizeof(c.api_provider), "anthropic");
    c.api_model[0] = '\0';     /* empty → per-provider default at spawn */
    c.api_key[0] = '\0';       /* empty → provider env var at spawn */
    c.api_base_url[0] = '\0';
    char model_path[sizeof(c.local_model_path)];
    if (liu_model_default_path(model_path, sizeof model_path) &&
        liu_model_file_ok(model_path)) {
        snprintf(c.local_model_path, sizeof(c.local_model_path), "%s",
                 model_path);
    }
    snprintf(c.source_lang, sizeof(c.source_lang), "Turkish");
    snprintf(c.target_lang, sizeof(c.target_lang), "English");
    c.tab_window_sec = 0.4f;
    c.active_in_claude   = true;
    c.active_in_codex    = true;
    c.active_in_opencode = true;
    c.active_in_grok     = true;
    return c;
}

i32 translate_language_count(void) {
    return (i32)(sizeof(g_translate_languages) / sizeof(g_translate_languages[0]));
}

const char *translate_language_name(i32 index) {
    i32 n = translate_language_count();
    if (index < 0 || index >= n) return "";
    return g_translate_languages[index];
}

void translate_cycle_language(char *lang, usize lang_cap) {
    if (!lang || lang_cap == 0) return;
    i32 n = translate_language_count();
    i32 cur = -1;
    for (i32 i = 0; i < n; i++) {
        if (strcmp(lang, g_translate_languages[i]) == 0) {
            cur = i;
            break;
        }
    }
    const char *next = g_translate_languages[(cur + 1 + n) % n];
    snprintf(lang, lang_cap, "%s", next);
}

void translate_normalize_direction(TranslateConfig *cfg) {
    if (!cfg) return;
    if (!cfg->source_lang[0]) {
        snprintf(cfg->source_lang, sizeof(cfg->source_lang), "Turkish");
    }
    if (!cfg->target_lang[0]) {
        snprintf(cfg->target_lang, sizeof(cfg->target_lang), "English");
    }
    if (strcmp(cfg->source_lang, cfg->target_lang) == 0) {
        const char *fallback = strcmp(cfg->source_lang, "English") == 0
            ? "Turkish" : "English";
        snprintf(cfg->target_lang, sizeof(cfg->target_lang), "%s", fallback);
    }
}

bool translate_active_in(const TranslateConfig *cfg, const char *agent_id) {
    if (!cfg || !agent_id || !agent_id[0]) return false;
    if (strcmp(agent_id, "claude") == 0)   return cfg->active_in_claude;
    if (strcmp(agent_id, "codex") == 0)    return cfg->active_in_codex;
    if (strcmp(agent_id, "opencode") == 0) return cfg->active_in_opencode;
    if (strcmp(agent_id, "grok") == 0)     return cfg->active_in_grok;
    return true;
}

void translate_build_prompt(const TranslateConfig *cfg,
                            const char *text,
                            char *out, usize out_cap) {
    if (!out || out_cap == 0) return;
    TranslateConfig normalized;
    if (cfg) {
        normalized = *cfg;
    } else {
        normalized = translate_config_default();
    }
    translate_normalize_direction(&normalized);
    const char *lang = normalized.target_lang;
    snprintf(out, out_cap,
             "Translate the following segment into %s, without additional explanation. %s",
             lang, text ? text : "");
}
