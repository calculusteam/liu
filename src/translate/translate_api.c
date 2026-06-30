/*
 * Liu - Translate-on-Tab: API backend (see translate_api.h).
 */
#include "translate/translate_api.h"

#include "cJSON.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#ifndef PLATFORM_WIN32
#include <sys/wait.h>
#endif

/* ----------------------------------------------------------------- *
 *  Provider table                                                    *
 * ----------------------------------------------------------------- */

typedef enum {
    API_PROV_ANTHROPIC = 0,
    API_PROV_OPENAI,
    API_PROV_OPENROUTER,
    API_PROV_CUSTOM,
    API_PROV_UNKNOWN,
} ApiProvider;

static ApiProvider provider_id(const char *p) {
    if (!p || !p[0])                  return API_PROV_ANTHROPIC; /* default */
    if (strcmp(p, "anthropic") == 0)  return API_PROV_ANTHROPIC;
    if (strcmp(p, "openai") == 0)     return API_PROV_OPENAI;
    if (strcmp(p, "openrouter") == 0) return API_PROV_OPENROUTER;
    if (strcmp(p, "custom") == 0)     return API_PROV_CUSTOM;
    return API_PROV_UNKNOWN;
}

const char *translate_api_provider_display(const char *provider) {
    switch (provider_id(provider)) {
    case API_PROV_ANTHROPIC:  return "Anthropic API";
    case API_PROV_OPENAI:     return "OpenAI API";
    case API_PROV_OPENROUTER: return "OpenRouter";
    case API_PROV_CUSTOM:     return "Custom API";
    default:                  return "API";
    }
}

const char *translate_api_default_model(const char *provider) {
    switch (provider_id(provider)) {
    case API_PROV_ANTHROPIC:  return "claude-haiku-4-5";
    case API_PROV_OPENAI:     return "gpt-5.4-mini";
    case API_PROV_OPENROUTER: return "anthropic/claude-haiku-4.5";
    default:                  return "";  /* custom: must be picked */
    }
}

const char *translate_api_effective_key(const TranslateConfig *cfg,
                                        char *buf, usize cap) {
    if (!buf || cap == 0) return "";
    buf[0] = '\0';
    if (!cfg) return buf;
    if (cfg->api_key[0]) {
        snprintf(buf, cap, "%s", cfg->api_key);
        return buf;
    }
    const char *env = NULL;
    switch (provider_id(cfg->api_provider)) {
    case API_PROV_ANTHROPIC:  env = getenv("ANTHROPIC_API_KEY");  break;
    case API_PROV_OPENAI:     env = getenv("OPENAI_API_KEY");     break;
    case API_PROV_OPENROUTER: env = getenv("OPENROUTER_API_KEY"); break;
    default: break;  /* custom: config-only, never env */
    }
    if (env && env[0]) snprintf(buf, cap, "%s", env);
    return buf;
}

bool translate_api_ready(const TranslateConfig *cfg,
                         char *why, usize why_cap) {
    if (why && why_cap) why[0] = '\0';
    if (!cfg) return false;
    ApiProvider prov = provider_id(cfg->api_provider);
    if (prov == API_PROV_UNKNOWN) {
        if (why) snprintf(why, why_cap, "Translate: unknown API provider");
        return false;
    }
    if (prov == API_PROV_CUSTOM) {
        if (!cfg->api_base_url[0]) {
            if (why) snprintf(why, why_cap,
                              "Translate: set API base URL in Settings");
            return false;
        }
        if (!cfg->api_model[0]) {
            if (why) snprintf(why, why_cap,
                              "Translate: pick an API model in Settings");
            return false;
        }
        return true;   /* local servers may be unauthenticated */
    }
    char key[256];
    translate_api_effective_key(cfg, key, sizeof key);
    if (!key[0]) {
        if (why) snprintf(why, why_cap,
                          "Translate: set API key in Settings");
        return false;
    }
    return true;
}

/* ----------------------------------------------------------------- *
 *  Request build + spawn                                             *
 * ----------------------------------------------------------------- */

/* Escape a string for a double-quoted curl-config value. curl's parser
 * understands \\ and \" inside quotes; the JSON body coming out of
 * cJSON_PrintUnformatted never contains raw control bytes (they're
 * \u-escaped inside JSON strings), so doubling backslashes and escaping
 * quotes is sufficient. Returns malloc'd string or NULL. */
static char *curlcfg_escape(const char *s) {
    usize n = strlen(s);
    char *out = (char *)malloc(n * 2 + 1);
    if (!out) return NULL;
    usize o = 0;
    for (usize i = 0; i < n; i++) {
        char c = s[i];
        if (c == '\\' || c == '"') out[o++] = '\\';
        out[o++] = c;
    }
    out[o] = '\0';
    return out;
}

/* Copy `s` into `out` dropping anything that could break out of a
 * double-quoted curl-config value: control bytes (a newline starts a new
 * config line — option injection), double quotes and backslashes. Keys,
 * URLs and model ids are plain ASCII tokens, so dropping is safe. The
 * Settings editors already filter these, but config.json can be edited
 * by hand and env vars are arbitrary. */
static void curlcfg_sanitize(const char *s, char *out, usize cap) {
    usize o = 0;
    for (const char *p = s; *p && o + 1 < cap; p++) {
        u8 c = (u8)*p;
        if (c < 32 || c == '"' || c == '\\') continue;
        out[o++] = (char)c;
    }
    out[o] = '\0';
}

/* "<base>/chat/completions" with trailing slashes trimmed. */
static void custom_chat_url(const char *base, char *out, usize cap) {
    usize blen = strlen(base);
    while (blen > 0 && base[blen - 1] == '/') blen--;
    snprintf(out, cap, "%.*s/chat/completions", (int)blen, base);
}

bool translate_api_spawn(const TranslateConfig *cfg,
                         const char *text,
                         TranslateAgentSpawn *out) {
    if (!cfg || !text || !out) return false;
    out->child_pid = 0;
    out->stdout_fd = -1;

    ApiProvider prov = provider_id(cfg->api_provider);
    if (prov == API_PROV_UNKNOWN) return false;

    char key_raw[256];
    translate_api_effective_key(cfg, key_raw, sizeof key_raw);
    char key[256];
    curlcfg_sanitize(key_raw, key, sizeof key);
    if (prov != API_PROV_CUSTOM && !key[0]) return false;

    const char *model = cfg->api_model[0]
        ? cfg->api_model
        : translate_api_default_model(cfg->api_provider);
    if (!model[0]) return false;

    /* Canonical translation prompt (same builder as the other backends). */
    usize prompt_cap = 64 * 1024;
    char *prompt = (char *)malloc(prompt_cap);
    if (!prompt) return false;
    translate_build_prompt(cfg, text, prompt, prompt_cap);

    /* JSON body. */
    cJSON *body = cJSON_CreateObject();
    if (!body) { free(prompt); return false; }
    cJSON_AddStringToObject(body, "model", model);
    if (prov == API_PROV_ANTHROPIC) {
        cJSON_AddNumberToObject(body, "max_tokens", 4096);
    }
    cJSON *messages = cJSON_AddArrayToObject(body, "messages");
    cJSON *msg = cJSON_CreateObject();
    if (!messages || !msg) {
        if (msg) cJSON_Delete(msg);
        cJSON_Delete(body);
        free(prompt);
        return false;
    }
    cJSON_AddStringToObject(msg, "role", "user");
    cJSON_AddStringToObject(msg, "content", prompt);
    cJSON_AddItemToArray(messages, msg);
    char *body_text = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    free(prompt);
    if (!body_text) return false;

    char *body_esc = curlcfg_escape(body_text);
    free(body_text);
    if (!body_esc) return false;

    /* Endpoint + auth header. */
    char url[512];
    char auth[320];
    const char *extra_header = NULL;
    switch (prov) {
    case API_PROV_ANTHROPIC:
        snprintf(url, sizeof url, "https://api.anthropic.com/v1/messages");
        snprintf(auth, sizeof auth, "x-api-key: %s", key);
        extra_header = "anthropic-version: 2023-06-01";
        break;
    case API_PROV_OPENAI:
        snprintf(url, sizeof url,
                 "https://api.openai.com/v1/chat/completions");
        snprintf(auth, sizeof auth, "Authorization: Bearer %s", key);
        break;
    case API_PROV_OPENROUTER:
        snprintf(url, sizeof url,
                 "https://openrouter.ai/api/v1/chat/completions");
        snprintf(auth, sizeof auth, "Authorization: Bearer %s", key);
        break;
    case API_PROV_CUSTOM:
    default: {
        char base[256];
        curlcfg_sanitize(cfg->api_base_url, base, sizeof base);
        custom_chat_url(base, url, sizeof url);
        if (key[0]) snprintf(auth, sizeof auth,
                             "Authorization: Bearer %s", key);
        else auth[0] = '\0';
        break;
    }
    }

    /* curl -K - config. `data` implies POST. max-time stays under
     * main.c's 30 s child timeout so curl reports its own error first. */
    usize cfg_cap = strlen(body_esc) + 1024;
    char *cfg_text = (char *)malloc(cfg_cap);
    if (!cfg_text) { free(body_esc); return false; }
    usize off = 0;
    i32 w = snprintf(cfg_text + off, cfg_cap - off,
                     "url = \"%s\"\n"
                     "silent\n"
                     "show-error\n"
                     "max-time = 28\n"
                     "header = \"content-type: application/json\"\n",
                     url);
    if (w > 0) off += (usize)w;
    if (auth[0] && off < cfg_cap) {
        w = snprintf(cfg_text + off, cfg_cap - off,
                     "header = \"%s\"\n", auth);
        if (w > 0) off += (usize)w;
    }
    if (extra_header && off < cfg_cap) {
        w = snprintf(cfg_text + off, cfg_cap - off,
                     "header = \"%s\"\n", extra_header);
        if (w > 0) off += (usize)w;
    }
    if (off < cfg_cap) {
        w = snprintf(cfg_text + off, cfg_cap - off,
                     "data = \"%s\"\n", body_esc);
        if (w > 0) off += (usize)w;
    }
    free(body_esc);
    if (off >= cfg_cap) {   /* truncated — refuse rather than send garbage */
        free(cfg_text);
        return false;
    }

    int pipefd[2];
    if (pipe(pipefd) != 0) { free(cfg_text); return false; }
    (void)fcntl(pipefd[0], F_SETFD, FD_CLOEXEC);
    (void)fcntl(pipefd[1], F_SETFD, FD_CLOEXEC);

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]); close(pipefd[1]); free(cfg_text);
        return false;
    }
    if (pid == 0) {
        /* Child — stdout+stderr into the capture pipe (finalize sorts the
         * JSON from any curl error text), config pumped over stdin by a
         * grandchild so pipe-buffer limits can't deadlock the exec. */
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);

        int sp[2];
        if (pipe(sp) == 0) {
            pid_t gc = fork();
            if (gc == 0) {
                close(sp[0]);
                (void)!write(sp[1], cfg_text, strlen(cfg_text));
                close(sp[1]);
                _exit(0);
            }
            close(sp[1]);
            dup2(sp[0], STDIN_FILENO);
            close(sp[0]);
        } else {
            _exit(127);
        }

        execlp("curl", "curl", "-K", "-", (char *)NULL);
        const char *err = "[liu] failed to exec curl\n";
        (void)!write(STDERR_FILENO, err, strlen(err));
        _exit(127);
    }

    close(pipefd[1]);
    int flags = fcntl(pipefd[0], F_GETFL, 0);
    fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);
    free(cfg_text);

    out->child_pid = (i32)pid;
    out->stdout_fd = pipefd[0];
    return true;
}

/* ----------------------------------------------------------------- *
 *  Response finalize                                                 *
 * ----------------------------------------------------------------- */

static void copy_trimmed(const char *s, char *out, usize out_cap) {
    usize start = 0, end = strlen(s);
    while (start < end && (u8)s[start] <= ' ') start++;
    while (end > start && (u8)s[end - 1] <= ' ') end--;
    usize n = end - start;
    if (n >= out_cap) n = out_cap - 1;
    memcpy(out, s + start, n);
    out[n] = '\0';
}

bool translate_api_finalize(const TranslateConfig *cfg,
                            const char *log, i32 log_len,
                            char *out, usize out_cap,
                            char *err, usize err_cap) {
    if (out && out_cap) out[0] = '\0';
    if (err && err_cap) err[0] = '\0';
    if (!log || log_len <= 0 || !out || out_cap == 0) return false;

    ApiProvider prov = cfg ? provider_id(cfg->api_provider)
                           : API_PROV_ANTHROPIC;

    /* curl noise (stderr is merged) may precede the JSON — parse from the
     * first '{'. cJSON ignores trailing bytes after the closing brace. */
    const char *start = memchr(log, '{', (usize)log_len);
    cJSON *root = NULL;
    if (start) {
        usize jlen = (usize)(log + log_len - start);
        char *scratch = (char *)malloc(jlen + 1);
        if (scratch) {
            memcpy(scratch, start, jlen);
            scratch[jlen] = '\0';
            root = cJSON_Parse(scratch);
            free(scratch);
        }
    }
    if (!root) {
        if (err && err_cap) {
            char tail[200];
            i32 tn = log_len < (i32)sizeof(tail) - 1 ? log_len
                                                     : (i32)sizeof(tail) - 1;
            memcpy(tail, log, (usize)tn);
            tail[tn] = '\0';
            copy_trimmed(tail, err, err_cap);
        }
        return false;
    }

    /* Error envelope first. Hosted providers use {"error":{"message": …}};
     * several OpenAI-compatible self-hosted servers (Ollama native,
     * llama.cpp/vLLM/LM Studio proxies) return {"error":"…"} as a plain
     * string instead — exactly the "custom" backend's targets, where a
     * clear message matters most. Handle both. */
    cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
    if (cJSON_IsObject(error)) {
        cJSON *m = cJSON_GetObjectItemCaseSensitive(error, "message");
        if (err && err_cap && cJSON_IsString(m)) {
            copy_trimmed(m->valuestring, err, err_cap);
        }
        cJSON_Delete(root);
        return false;
    }
    if (cJSON_IsString(error)) {
        if (err && err_cap) copy_trimmed(error->valuestring, err, err_cap);
        cJSON_Delete(root);
        return false;
    }

    bool got = false;
    if (prov == API_PROV_ANTHROPIC) {
        /* {"content":[{"type":"text","text": …}, …]} */
        cJSON *content = cJSON_GetObjectItemCaseSensitive(root, "content");
        if (cJSON_IsArray(content)) {
            usize o = 0;
            cJSON *block;
            cJSON_ArrayForEach(block, content) {
                cJSON *type = cJSON_GetObjectItemCaseSensitive(block, "type");
                cJSON *txt  = cJSON_GetObjectItemCaseSensitive(block, "text");
                if (cJSON_IsString(type) &&
                    strcmp(type->valuestring, "text") == 0 &&
                    cJSON_IsString(txt)) {
                    usize n = strlen(txt->valuestring);
                    if (o + n >= out_cap) n = out_cap - 1 - o;
                    memcpy(out + o, txt->valuestring, n);
                    o += n;
                    if (o >= out_cap - 1) break;
                }
            }
            out[o] = '\0';
            got = o > 0;
        }
    } else {
        /* {"choices":[{"message":{"content": …}}]} */
        cJSON *choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
        cJSON *first = cJSON_IsArray(choices)
            ? cJSON_GetArrayItem(choices, 0) : NULL;
        cJSON *message = first
            ? cJSON_GetObjectItemCaseSensitive(first, "message") : NULL;
        cJSON *content = message
            ? cJSON_GetObjectItemCaseSensitive(message, "content") : NULL;
        if (cJSON_IsString(content)) {
            snprintf(out, out_cap, "%s", content->valuestring);
            got = out[0] != '\0';
        }
    }

    if (got) {
        /* In-place trim (operates on `out`, root no longer needed for it). */
        char *tmp = (char *)malloc(strlen(out) + 1);
        if (tmp) {
            strcpy(tmp, out);
            copy_trimmed(tmp, out, out_cap);
            free(tmp);
        }
        got = out[0] != '\0';
    }
    /* No content parsed — surface a top-level {"message"|"detail": …}
     * (various gateways) before falling back to the generic message.
     * Must run before cJSON_Delete(root). */
    if (!got && err && err_cap && !err[0]) {
        cJSON *top = cJSON_GetObjectItemCaseSensitive(root, "message");
        if (!cJSON_IsString(top))
            top = cJSON_GetObjectItemCaseSensitive(root, "detail");
        if (cJSON_IsString(top)) copy_trimmed(top->valuestring, err, err_cap);
    }
    cJSON_Delete(root);
    if (!got && err && err_cap && !err[0]) {
        snprintf(err, err_cap, "empty response");
    }
    return got;
}
