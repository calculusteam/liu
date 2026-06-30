/*
 * Liu - Translate model catalog (see model_catalog.h for the contract).
 *
 * Fetch children mirror translate_agent_spawn's fork+exec shape: argv-only
 * exec (no shell), CLOEXEC pipe ends, non-blocking parent read, WNOHANG
 * reap from the tick. Secrets ride a `curl -K -` stdin config pumped by a
 * grandchild — an API key never appears in `ps` output.
 */
#include "translate/model_catalog.h"

#include "core/agent_detect.h"
#include "cJSON.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifndef PLATFORM_WIN32
#include <sys/wait.h>
#endif

/* ----------------------------------------------------------------- *
 *  Source table                                                      *
 * ----------------------------------------------------------------- */

enum {
    SRC_CLAUDE = 0,      /* models.dev: anthropic            */
    SRC_OPENCODE,        /* models.dev: opencode-go          */
    SRC_CODEX,           /* models.dev: openai               */
    SRC_GROK,            /* `grok models`                    */
    SRC_API_ANTHROPIC,   /* GET api.anthropic.com/v1/models  */
    SRC_API_OPENAI,      /* GET api.openai.com/v1/models     */
    SRC_API_OPENROUTER,  /* GET openrouter.ai/api/v1/models  */
    SRC_API_CUSTOM,      /* GET <base>/models                */
    SRC_COUNT,
};

/* Job kinds — one curl/grok child can fill several sources. */
enum {
    JOB_MODELSDEV = 0,   /* fills CLAUDE + OPENCODE + CODEX  */
    JOB_GROK,
    JOB_API_ANTHROPIC,
    JOB_API_OPENAI,
    JOB_API_OPENROUTER,
    JOB_API_CUSTOM,
    JOB_COUNT,
};

#define CATALOG_TTL_AGENT_SEC   (24 * 60 * 60)
#define CATALOG_TTL_API_SEC     (60 * 60)
#define CATALOG_RETRY_SEC       60
#define CATALOG_JOB_TIMEOUT_SEC 35.0
#define CATALOG_BUF_HARD_CAP    (16u * 1024u * 1024u)

static ModelList g_lists[SRC_COUNT];
static long long g_last_attempt[JOB_COUNT];
/* djb2 of key+base per API source — param change invalidates the list. */
static u32       g_api_fingerprint[SRC_COUNT];
static bool      g_cache_loaded = false;

typedef struct {
    i32    kind;          /* JOB_*; -1 = slot free */
    pid_t  pid;
    int    fd;            /* non-blocking stdout pipe; -1 = none */
    char  *buf;
    usize  len, cap;
    time_t started;
} CatalogJob;

static CatalogJob g_jobs[4] = {
    {-1, 0, -1, NULL, 0, 0, 0}, {-1, 0, -1, NULL, 0, 0, 0},
    {-1, 0, -1, NULL, 0, 0, 0}, {-1, 0, -1, NULL, 0, 0, 0},
};

static const char *src_cache_name(i32 src) {
    switch (src) {
    case SRC_CLAUDE:   return "claude";
    case SRC_OPENCODE: return "opencode";
    case SRC_CODEX:    return "codex";
    case SRC_GROK:     return "grok";
    default:           return NULL;   /* API lists are not persisted */
    }
}

static u32 catalog_hash(const char *a, const char *b) {
    u32 h = 5381;
    for (const char *p = a; p && *p; p++) h = h * 33u + (u8)*p;
    h = h * 33u + '|';
    for (const char *p = b; p && *p; p++) h = h * 33u + (u8)*p;
    return h;
}

/* Drop anything that could break out of a double-quoted curl-config
 * value (a newline starts a new config line — option injection): control
 * bytes, double quotes, backslashes. Keys and URLs are ASCII tokens, so
 * dropping is safe. Mirrors translate_api.c's sanitizer. */
static void catalog_sanitize(const char *s, char *out, usize cap) {
    usize o = 0;
    for (const char *p = s; p && *p && o + 1 < cap; p++) {
        u8 c = (u8)*p;
        if (c < 32 || c == '"' || c == '\\') continue;
        out[o++] = (char)c;
    }
    out[o] = '\0';
}

/* ----------------------------------------------------------------- *
 *  Disk cache (~/.config/Liu/agent_models.json) — agent sources only *
 * ----------------------------------------------------------------- */

static bool catalog_cache_path(char *out, usize cap) {
    const char *home = getenv("HOME");
    if (!home || !home[0]) return false;
    snprintf(out, cap, "%s/.config/Liu/agent_models.json", home);
    return true;
}

static void catalog_cache_load(void) {
    if (g_cache_loaded) return;
    g_cache_loaded = true;

    char path[1024];
    if (!catalog_cache_path(path, sizeof path)) return;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 1024 * 1024) { fclose(f); return; }
    char *body = (char *)malloc((usize)sz + 1);
    if (!body) { fclose(f); return; }
    usize rd = fread(body, 1, (usize)sz, f);
    fclose(f);
    body[rd] = '\0';

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) return;
    for (i32 src = 0; src < SRC_COUNT; src++) {
        const char *name = src_cache_name(src);
        if (!name) continue;
        cJSON *entry = cJSON_GetObjectItemCaseSensitive(root, name);
        if (!cJSON_IsObject(entry)) continue;
        cJSON *ts  = cJSON_GetObjectItemCaseSensitive(entry, "fetched_at");
        cJSON *ids = cJSON_GetObjectItemCaseSensitive(entry, "ids");
        if (!cJSON_IsNumber(ts) || !cJSON_IsArray(ids)) continue;
        ModelList *l = &g_lists[src];
        l->count = 0;
        cJSON *id;
        cJSON_ArrayForEach(id, ids) {
            if (!cJSON_IsString(id) || !id->valuestring[0]) continue;
            if (l->count >= MODEL_CATALOG_MAX) break;
            snprintf(l->ids[l->count], MODEL_ID_CAP, "%s", id->valuestring);
            l->count++;
        }
        if (l->count > 0) {
            l->fetched_at = (long long)ts->valuedouble;
            l->state = MODEL_LIST_READY;
        }
    }
    cJSON_Delete(root);
}

static void catalog_cache_save(void) {
    char path[1024];
    if (!catalog_cache_path(path, sizeof path)) return;

    cJSON *root = cJSON_CreateObject();
    if (!root) return;
    for (i32 src = 0; src < SRC_COUNT; src++) {
        const char *name = src_cache_name(src);
        if (!name) continue;
        const ModelList *l = &g_lists[src];
        /* Persist any non-empty list — a READY fetch OR a stale-but-shown
         * FAILED one (a partial models.dev outage keeps the old ids in
         * memory via count>0; the on-disk cache must survive it too, or a
         * restart during the outage loses a list that's still rendered). */
        bool persistable = l->count > 0 &&
                           (l->state == MODEL_LIST_READY ||
                            l->state == MODEL_LIST_FAILED);
        if (!persistable) continue;
        cJSON *entry = cJSON_AddObjectToObject(root, name);
        if (!entry) continue;
        cJSON_AddNumberToObject(entry, "fetched_at", (double)l->fetched_at);
        cJSON *ids = cJSON_AddArrayToObject(entry, "ids");
        if (!ids) continue;
        for (i32 i = 0; i < l->count; i++) {
            cJSON_AddItemToArray(ids, cJSON_CreateString(l->ids[i]));
        }
    }
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text) return;

    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(text, 1, strlen(text), f);
        fclose(f);
    }
    free(text);
}

/* ----------------------------------------------------------------- *
 *  Spawn helpers                                                     *
 * ----------------------------------------------------------------- */

/* fork+exec `argv`, optionally pumping `stdin_body` into the child's
 * stdin via a grandchild (same pattern as translate_agent_spawn's
 * stdin_prompt path — immune to pipe-buffer limits). stderr goes to
 * /dev/null: these children's stdout is machine-parsed. */
static bool catalog_spawn(char *const argv[], const char *stdin_body,
                          pid_t *out_pid, int *out_fd) {
    int pipefd[2];
    if (pipe(pipefd) != 0) return false;
    (void)fcntl(pipefd[0], F_SETFD, FD_CLOEXEC);
    (void)fcntl(pipefd[1], F_SETFD, FD_CLOEXEC);

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]); close(pipefd[1]);
        return false;
    }
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) { dup2(devnull, STDERR_FILENO); }

        if (stdin_body && stdin_body[0]) {
            int sp[2];
            if (pipe(sp) == 0) {
                pid_t gc = fork();
                if (gc == 0) {
                    close(sp[0]);
                    (void)!write(sp[1], stdin_body, strlen(stdin_body));
                    close(sp[1]);
                    _exit(0);
                }
                close(sp[1]);
                dup2(sp[0], STDIN_FILENO);
                close(sp[0]);
            } else if (devnull >= 0) {
                dup2(devnull, STDIN_FILENO);
            }
        } else if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
        }
        if (devnull >= 0) close(devnull);

        execvp(argv[0], argv);
        _exit(127);
    }

    close(pipefd[1]);
    int flags = fcntl(pipefd[0], F_GETFL, 0);
    fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);
    *out_pid = pid;
    *out_fd = pipefd[0];
    return true;
}

static CatalogJob *catalog_job_slot(void) {
    for (usize i = 0; i < sizeof(g_jobs) / sizeof(g_jobs[0]); i++) {
        if (g_jobs[i].kind < 0) return &g_jobs[i];
    }
    return NULL;
}

static bool catalog_job_running(i32 kind) {
    for (usize i = 0; i < sizeof(g_jobs) / sizeof(g_jobs[0]); i++) {
        if (g_jobs[i].kind == kind) return true;
    }
    return false;
}

/* Build the `curl -K -` stdin config for an authenticated GET. The key
 * lives only in this string (child stdin), never on argv. */
static void catalog_curl_config(char *out, usize cap, const char *url,
                                const char *header1, const char *header2) {
    usize off = 0;
    i32 w = snprintf(out + off, cap - off,
                     "url = \"%s\"\nsilent\nmax-time = 25\nlocation\n", url);
    if (w > 0) off += (usize)w;
    if (off >= cap) { out[cap - 1] = '\0'; return; }
    if (header1 && header1[0]) {
        w = snprintf(out + off, cap - off, "header = \"%s\"\n", header1);
        if (w > 0) off += (usize)w;
        if (off >= cap) { out[cap - 1] = '\0'; return; }
    }
    if (header2 && header2[0]) {
        (void)snprintf(out + off, cap - off, "header = \"%s\"\n", header2);
    }
}

/* Stash for params captured at request time, consumed at spawn. */
static char g_api_key_scratch[2][320];     /* "x-api-key: …" / "Authorization: Bearer …" */
static char g_custom_models_url[320];

static bool catalog_job_start(i32 kind) {
    CatalogJob *job = catalog_job_slot();
    if (!job) return false;

    pid_t pid = 0;
    int fd = -1;
    bool ok = false;

    switch (kind) {
    case JOB_MODELSDEV: {
        char *argv[] = {(char *)"curl", (char *)"-sL", (char *)"--max-time",
                        (char *)"25", (char *)"https://models.dev/api.json",
                        NULL};
        ok = catalog_spawn(argv, NULL, &pid, &fd);
        break;
    }
    case JOB_GROK: {
        /* Resolve the grok binary the same way the translate spawn does —
         * PATH plus the curated GUI-launch fallback dirs. */
        AgentInfo agents[AGENT_MAX] = {0};
        i32 n = agent_detect_available(agents, AGENT_MAX);
        const char *grok_path = NULL;
        for (i32 i = 0; i < n; i++) {
            if (strcmp(agents[i].id, "grok") == 0) {
                grok_path = agents[i].path[0] ? agents[i].path
                                              : agents[i].binary;
                break;
            }
        }
        if (!grok_path) return false;
        char *argv[] = {(char *)grok_path, (char *)"models", NULL};
        ok = catalog_spawn(argv, NULL, &pid, &fd);
        break;
    }
    case JOB_API_ANTHROPIC: {
        char cfg[1024];
        catalog_curl_config(cfg, sizeof cfg,
                            "https://api.anthropic.com/v1/models?limit=100",
                            g_api_key_scratch[0],
                            "anthropic-version: 2023-06-01");
        char *argv[] = {(char *)"curl", (char *)"-K", (char *)"-", NULL};
        ok = catalog_spawn(argv, cfg, &pid, &fd);
        break;
    }
    case JOB_API_OPENAI: {
        char cfg[1024];
        catalog_curl_config(cfg, sizeof cfg,
                            "https://api.openai.com/v1/models",
                            g_api_key_scratch[1], NULL);
        char *argv[] = {(char *)"curl", (char *)"-K", (char *)"-", NULL};
        ok = catalog_spawn(argv, cfg, &pid, &fd);
        break;
    }
    case JOB_API_OPENROUTER: {
        char *argv[] = {(char *)"curl", (char *)"-sL", (char *)"--max-time",
                        (char *)"25",
                        (char *)"https://openrouter.ai/api/v1/models", NULL};
        ok = catalog_spawn(argv, NULL, &pid, &fd);
        break;
    }
    case JOB_API_CUSTOM: {
        if (!g_custom_models_url[0]) return false;
        char cfg[1024];
        catalog_curl_config(cfg, sizeof cfg, g_custom_models_url,
                            g_api_key_scratch[1][0] ? g_api_key_scratch[1]
                                                    : NULL,
                            NULL);
        char *argv[] = {(char *)"curl", (char *)"-K", (char *)"-", NULL};
        ok = catalog_spawn(argv, cfg, &pid, &fd);
        break;
    }
    default:
        return false;
    }

    if (!ok) return false;
    job->kind = kind;
    job->pid = pid;
    job->fd = fd;
    job->buf = NULL;
    job->len = 0;
    job->cap = 0;
    job->started = time(NULL);
    return true;
}

/* ----------------------------------------------------------------- *
 *  Parsers                                                           *
 * ----------------------------------------------------------------- */

typedef struct {
    char id[MODEL_ID_CAP];
    char sort_key[24];   /* release_date / created — desc order */
} ModelEntry;

static int model_entry_cmp_desc(const void *a, const void *b) {
    const ModelEntry *ea = (const ModelEntry *)a;
    const ModelEntry *eb = (const ModelEntry *)b;
    int c = strcmp(eb->sort_key, ea->sort_key);
    if (c != 0) return c;
    return strcmp(ea->id, eb->id);
}

static void list_commit(ModelList *l, ModelEntry *entries, i32 n,
                        bool sort_desc) {
    if (n <= 0) { l->state = MODEL_LIST_FAILED; return; }
    if (sort_desc) {
        qsort(entries, (usize)n, sizeof(ModelEntry), model_entry_cmp_desc);
    }
    if (n > MODEL_CATALOG_MAX) n = MODEL_CATALOG_MAX;
    for (i32 i = 0; i < n; i++) {
        snprintf(l->ids[i], MODEL_ID_CAP, "%s", entries[i].id);
    }
    l->count = n;
    l->state = MODEL_LIST_READY;
    l->fetched_at = (long long)time(NULL);
}

/* models.dev: { "<provider>": { "models": { "<id>": {"release_date": …} } } }.
 * `id_prefix` filters; `store_prefix` prepends (opencode-go/<id>). */
static void parse_modelsdev_provider(cJSON *root, const char *provider,
                                     const char *id_prefix,
                                     const char *id_substr_alt,
                                     const char *store_prefix,
                                     ModelList *out) {
    cJSON *prov = cJSON_GetObjectItemCaseSensitive(root, provider);
    cJSON *models = prov ? cJSON_GetObjectItemCaseSensitive(prov, "models")
                         : NULL;
    if (!cJSON_IsObject(models)) { out->state = MODEL_LIST_FAILED; return; }

    ModelEntry entries[128];
    i32 n = 0;
    cJSON *m;
    cJSON_ArrayForEach(m, models) {
        const char *id = m->string;
        if (!id || !id[0]) continue;
        bool match = true;
        if (id_prefix && strncmp(id, id_prefix, strlen(id_prefix)) != 0) {
            match = (id_substr_alt && strstr(id, id_substr_alt) != NULL);
        }
        if (!match) continue;
        if (n >= (i32)(sizeof(entries) / sizeof(entries[0]))) break;
        ModelEntry *e = &entries[n];
        if (store_prefix && store_prefix[0]) {
            snprintf(e->id, sizeof e->id, "%s/%s", store_prefix, id);
        } else {
            snprintf(e->id, sizeof e->id, "%s", id);
        }
        e->sort_key[0] = '\0';
        cJSON *rd = cJSON_GetObjectItemCaseSensitive(m, "release_date");
        if (cJSON_IsString(rd)) {
            snprintf(e->sort_key, sizeof e->sort_key, "%s", rd->valuestring);
        }
        n++;
    }
    list_commit(out, entries, n, true);
}

static void parse_modelsdev(const char *body) {
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        g_lists[SRC_CLAUDE].state   = MODEL_LIST_FAILED;
        g_lists[SRC_OPENCODE].state = MODEL_LIST_FAILED;
        g_lists[SRC_CODEX].state    = MODEL_LIST_FAILED;
        return;
    }
    parse_modelsdev_provider(root, "anthropic", "claude-", NULL, NULL,
                             &g_lists[SRC_CLAUDE]);
    /* OpenCode is intentionally the Go plan's catalog only — the broader
     * zen/free providers are excluded by request. */
    parse_modelsdev_provider(root, "opencode-go", NULL, NULL, "opencode-go",
                             &g_lists[SRC_OPENCODE]);
    parse_modelsdev_provider(root, "openai", "gpt-5", "codex", NULL,
                             &g_lists[SRC_CODEX]);
    cJSON_Delete(root);
    catalog_cache_save();
}

/* `grok models` →
 *   Available models:
 *     * grok-composer-2.5-fast (default)
 *     - grok-build                       */
static void parse_grok(const char *body) {
    ModelEntry entries[32];
    i32 n = 0;
    const char *p = body;
    while (p && *p && n < (i32)(sizeof(entries) / sizeof(entries[0]))) {
        const char *eol = strchr(p, '\n');
        usize linelen = eol ? (usize)(eol - p) : strlen(p);
        const char *s = p;
        const char *end = p + linelen;
        while (s < end && (*s == ' ' || *s == '\t')) s++;
        if (s + 2 < end && (*s == '*' || *s == '-') && s[1] == ' ') {
            s += 2;
            while (s < end && *s == ' ') s++;
            const char *tok = s;
            while (s < end && *s > ' ') s++;
            usize toklen = (usize)(s - tok);
            if (toklen > 0 && toklen < MODEL_ID_CAP) {
                ModelEntry *e = &entries[n++];
                memcpy(e->id, tok, toklen);
                e->id[toklen] = '\0';
                e->sort_key[0] = '\0';
            }
        }
        p = eol ? eol + 1 : NULL;
    }
    list_commit(&g_lists[SRC_GROK], entries, n, false);
    catalog_cache_save();
}

/* OpenAI-style {"data":[{"id": …, "created": …}]} — also matches
 * Anthropic's /v1/models and OpenRouter's public list. */
static void parse_api_models(const char *body, i32 src) {
    ModelList *out = &g_lists[src];
    cJSON *root = cJSON_Parse(body);
    if (!root) { out->state = MODEL_LIST_FAILED; return; }
    cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (!cJSON_IsArray(data)) {
        cJSON_Delete(root);
        out->state = MODEL_LIST_FAILED;
        return;
    }

    /* OpenRouter lists hundreds of models — keep the recognizable chat
     * families so 24 slots stay useful. */
    static const char *or_prefixes[] = {
        "anthropic/", "openai/", "google/", "x-ai/", "deepseek/",
        "moonshotai/", "qwen/", "mistralai/", "meta-llama/",
    };

    ModelEntry entries[512];
    i32 n = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, data) {
        cJSON *idj = cJSON_GetObjectItemCaseSensitive(item, "id");
        if (!cJSON_IsString(idj) || !idj->valuestring[0]) continue;
        const char *id = idj->valuestring;

        if (src == SRC_API_OPENAI) {
            bool keep = strncmp(id, "gpt-", 4) == 0 || strstr(id, "codex");
            /* Modality endpoints can't run chat/completions. */
            static const char *deny[] = {
                "audio", "realtime", "image", "tts", "transcribe",
                "embed", "moderation", "dall-e", "whisper", "search",
            };
            for (usize d = 0; keep && d < sizeof(deny) / sizeof(deny[0]); d++) {
                if (strstr(id, deny[d])) keep = false;
            }
            if (!keep) continue;
        } else if (src == SRC_API_ANTHROPIC) {
            if (strncmp(id, "claude-", 7) != 0) continue;
        } else if (src == SRC_API_OPENROUTER) {
            bool keep = false;
            for (usize d = 0; d < sizeof(or_prefixes) / sizeof(or_prefixes[0]); d++) {
                if (strncmp(id, or_prefixes[d], strlen(or_prefixes[d])) == 0) {
                    keep = true;
                    break;
                }
            }
            if (!keep) continue;
        }

        if (n >= (i32)(sizeof(entries) / sizeof(entries[0]))) break;
        if (strlen(id) >= MODEL_ID_CAP) continue;
        ModelEntry *e = &entries[n];
        snprintf(e->id, sizeof e->id, "%s", id);
        e->sort_key[0] = '\0';
        cJSON *created = cJSON_GetObjectItemCaseSensitive(item, "created");
        if (cJSON_IsNumber(created)) {
            /* Zero-padded so lexicographic == numeric. */
            snprintf(e->sort_key, sizeof e->sort_key, "%020.0f",
                     created->valuedouble);
        } else {
            cJSON *created_at = cJSON_GetObjectItemCaseSensitive(item, "created_at");
            if (cJSON_IsString(created_at)) {
                snprintf(e->sort_key, sizeof e->sort_key, "%s",
                         created_at->valuestring);
            }
        }
        n++;
    }
    cJSON_Delete(root);
    /* Custom (ollama & friends) lists are small and unordered — keep the
     * server's order; sort the hosted providers newest-first. */
    list_commit(out, entries, n, src != SRC_API_CUSTOM);
}

static void catalog_job_finish(CatalogJob *job, bool exited_ok) {
    const char *body = job->buf ? job->buf : "";
    switch (job->kind) {
    case JOB_MODELSDEV:
        if (exited_ok) parse_modelsdev(body);
        else {
            if (g_lists[SRC_CLAUDE].state == MODEL_LIST_FETCHING)
                g_lists[SRC_CLAUDE].state = MODEL_LIST_FAILED;
            if (g_lists[SRC_OPENCODE].state == MODEL_LIST_FETCHING)
                g_lists[SRC_OPENCODE].state = MODEL_LIST_FAILED;
            if (g_lists[SRC_CODEX].state == MODEL_LIST_FETCHING)
                g_lists[SRC_CODEX].state = MODEL_LIST_FAILED;
        }
        break;
    case JOB_GROK:
        if (exited_ok) parse_grok(body);
        else g_lists[SRC_GROK].state = MODEL_LIST_FAILED;
        break;
    case JOB_API_ANTHROPIC:
        if (exited_ok) parse_api_models(body, SRC_API_ANTHROPIC);
        else g_lists[SRC_API_ANTHROPIC].state = MODEL_LIST_FAILED;
        break;
    case JOB_API_OPENAI:
        if (exited_ok) parse_api_models(body, SRC_API_OPENAI);
        else g_lists[SRC_API_OPENAI].state = MODEL_LIST_FAILED;
        break;
    case JOB_API_OPENROUTER:
        if (exited_ok) parse_api_models(body, SRC_API_OPENROUTER);
        else g_lists[SRC_API_OPENROUTER].state = MODEL_LIST_FAILED;
        break;
    case JOB_API_CUSTOM:
        if (exited_ok) parse_api_models(body, SRC_API_CUSTOM);
        else g_lists[SRC_API_CUSTOM].state = MODEL_LIST_FAILED;
        break;
    default:
        break;
    }
}

/* A FAILED parse may have left stale-but-usable ids around (e.g. a READY
 * list re-fetched after TTL whose refresh failed). Keep showing the old
 * ids in that case: FAILED + count>0 renders as the cached list. */

/* ----------------------------------------------------------------- *
 *  Public API                                                        *
 * ----------------------------------------------------------------- */

void model_catalog_shutdown(void) {
    for (usize i = 0; i < sizeof(g_jobs) / sizeof(g_jobs[0]); i++) {
        CatalogJob *job = &g_jobs[i];
        if (job->kind < 0) continue;
        /* SIGKILL, not SIGTERM: `grok models` has no --max-time and could
         * otherwise outlive the app as an orphan reparented to launchd. */
        kill(job->pid, SIGKILL);
        int st;
        while (waitpid(job->pid, &st, 0) < 0 && errno == EINTR) { }
        if (job->fd >= 0) { close(job->fd); job->fd = -1; }
        free(job->buf);
        job->buf = NULL;
        job->len = job->cap = 0;
        job->kind = -1;
        job->pid = 0;
    }
}

void model_catalog_tick(void) {
    for (usize ji = 0; ji < sizeof(g_jobs) / sizeof(g_jobs[0]); ji++) {
        CatalogJob *job = &g_jobs[ji];
        if (job->kind < 0) continue;

        /* Drain whatever the child produced since last frame. */
        if (job->fd >= 0) {
            char chunk[4096];
            for (;;) {
                ssize_t n = read(job->fd, chunk, sizeof chunk);
                if (n <= 0) break;
                if (job->len + (usize)n + 1 > job->cap) {
                    usize want = job->cap ? job->cap * 2 : 64 * 1024;
                    while (want < job->len + (usize)n + 1) want *= 2;
                    if (want > CATALOG_BUF_HARD_CAP) {
                        want = CATALOG_BUF_HARD_CAP;
                    }
                    if (job->len + (usize)n + 1 > want) {
                        /* Body exceeds the hard cap — drop the fetch. */
                        kill(job->pid, SIGKILL);
                        break;
                    }
                    char *nb = (char *)realloc(job->buf, want);
                    if (!nb) { kill(job->pid, SIGKILL); break; }
                    job->buf = nb;
                    job->cap = want;
                }
                memcpy(job->buf + job->len, chunk, (usize)n);
                job->len += (usize)n;
                job->buf[job->len] = '\0';
            }
        }

        /* Timeout guard — curl has its own --max-time but `grok models`
         * does not; don't let a hung child pin the slot forever. */
        if (difftime(time(NULL), job->started) > CATALOG_JOB_TIMEOUT_SEC) {
            kill(job->pid, SIGKILL);
        }

        int wstatus = 0;
        pid_t wp = waitpid(job->pid, &wstatus, WNOHANG);
        if (wp != job->pid) continue;

        /* Final drain after exit. */
        if (job->fd >= 0) {
            char chunk[4096];
            for (;;) {
                ssize_t n = read(job->fd, chunk, sizeof chunk);
                if (n <= 0) break;
                if (job->len + (usize)n + 1 > job->cap) {
                    usize want = job->cap ? job->cap * 2 : 64 * 1024;
                    while (want < job->len + (usize)n + 1) want *= 2;
                    if (want > CATALOG_BUF_HARD_CAP) break;
                    char *nb = (char *)realloc(job->buf, want);
                    if (!nb) break;
                    job->buf = nb;
                    job->cap = want;
                }
                if (job->len + (usize)n + 1 > job->cap) break;
                memcpy(job->buf + job->len, chunk, (usize)n);
                job->len += (usize)n;
                job->buf[job->len] = '\0';
            }
            close(job->fd);
            job->fd = -1;
        }

        bool exited_ok = WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0 &&
                         job->len > 0;
        catalog_job_finish(job, exited_ok);

        free(job->buf);
        job->buf = NULL;
        job->len = job->cap = 0;
        job->kind = -1;
        job->pid = 0;
    }
}

/* Shared request gate: fresh/READY → no-op; FETCHING → no-op; throttle
 * retries after failure. Marks the sources FETCHING when a job starts. */
static bool catalog_should_fetch(const ModelList *l, i32 job_kind,
                                 long long ttl_sec) {
    long long now = (long long)time(NULL);
    if (catalog_job_running(job_kind)) return false;
    if (l->state == MODEL_LIST_READY &&
        now - l->fetched_at < ttl_sec) return false;
    if (now - g_last_attempt[job_kind] < CATALOG_RETRY_SEC) return false;
    return true;
}

void model_catalog_request_agent(const char *agent_id) {
    if (!agent_id || !agent_id[0]) return;
    catalog_cache_load();

    if (strcmp(agent_id, "grok") == 0) {
        if (!catalog_should_fetch(&g_lists[SRC_GROK], JOB_GROK,
                                  CATALOG_TTL_AGENT_SEC)) return;
        g_last_attempt[JOB_GROK] = (long long)time(NULL);
        if (catalog_job_start(JOB_GROK)) {
            g_lists[SRC_GROK].state = MODEL_LIST_FETCHING;
        } else {
            g_lists[SRC_GROK].state = MODEL_LIST_FAILED;
        }
        return;
    }

    i32 src;
    if (strcmp(agent_id, "claude") == 0)        src = SRC_CLAUDE;
    else if (strcmp(agent_id, "opencode") == 0) src = SRC_OPENCODE;
    else if (strcmp(agent_id, "codex") == 0)    src = SRC_CODEX;
    else return;

    if (!catalog_should_fetch(&g_lists[src], JOB_MODELSDEV,
                              CATALOG_TTL_AGENT_SEC)) return;
    g_last_attempt[JOB_MODELSDEV] = (long long)time(NULL);
    if (catalog_job_start(JOB_MODELSDEV)) {
        /* One download feeds all three models.dev-backed lists. */
        if (g_lists[SRC_CLAUDE].state != MODEL_LIST_READY)
            g_lists[SRC_CLAUDE].state = MODEL_LIST_FETCHING;
        if (g_lists[SRC_OPENCODE].state != MODEL_LIST_READY)
            g_lists[SRC_OPENCODE].state = MODEL_LIST_FETCHING;
        if (g_lists[SRC_CODEX].state != MODEL_LIST_READY)
            g_lists[SRC_CODEX].state = MODEL_LIST_FETCHING;
        g_lists[src].state = MODEL_LIST_FETCHING;
    } else {
        g_lists[src].state = MODEL_LIST_FAILED;
    }
}

void model_catalog_request_api(const char *provider, const char *api_key,
                               const char *base_url) {
    if (!provider || !provider[0]) return;
    catalog_cache_load();

    i32 src, job;
    if (strcmp(provider, "anthropic") == 0)       { src = SRC_API_ANTHROPIC;  job = JOB_API_ANTHROPIC; }
    else if (strcmp(provider, "openai") == 0)     { src = SRC_API_OPENAI;     job = JOB_API_OPENAI; }
    else if (strcmp(provider, "openrouter") == 0) { src = SRC_API_OPENROUTER; job = JOB_API_OPENROUTER; }
    else if (strcmp(provider, "custom") == 0)     { src = SRC_API_CUSTOM;     job = JOB_API_CUSTOM; }
    else return;

    /* Hosted providers can't list models without a key. */
    bool needs_key = (src == SRC_API_ANTHROPIC || src == SRC_API_OPENAI);
    if (needs_key && (!api_key || !api_key[0])) {
        return;   /* stay IDLE — UI shows the static fallback */
    }
    if (src == SRC_API_CUSTOM && (!base_url || !base_url[0])) return;

    /* Key/base change invalidates whatever was fetched before. */
    u32 fp = catalog_hash(api_key ? api_key : "", base_url ? base_url : "");
    if (fp != g_api_fingerprint[src]) {
        g_api_fingerprint[src] = fp;
        g_lists[src].state = MODEL_LIST_IDLE;
        g_lists[src].count = 0;
        g_lists[src].fetched_at = 0;
        g_last_attempt[job] = 0;
    }

    if (!catalog_should_fetch(&g_lists[src], job, CATALOG_TTL_API_SEC)) return;

    /* Stage params for the spawn — sanitized so a crafted key/URL from a
     * hand-edited config.json can't inject extra curl-config lines. */
    char key_san[256];
    catalog_sanitize(api_key ? api_key : "", key_san, sizeof key_san);
    if (src == SRC_API_ANTHROPIC) {
        snprintf(g_api_key_scratch[0], sizeof g_api_key_scratch[0],
                 "x-api-key: %s", key_san);
    } else if (key_san[0]) {
        snprintf(g_api_key_scratch[1], sizeof g_api_key_scratch[1],
                 "Authorization: Bearer %s", key_san);
    } else {
        g_api_key_scratch[1][0] = '\0';
    }
    if (src == SRC_API_CUSTOM) {
        char base_san[256];
        catalog_sanitize(base_url, base_san, sizeof base_san);
        usize blen = strlen(base_san);
        while (blen > 0 && base_san[blen - 1] == '/') blen--;
        snprintf(g_custom_models_url, sizeof g_custom_models_url,
                 "%.*s/models", (int)blen, base_san);
    }

    g_last_attempt[job] = (long long)time(NULL);
    if (catalog_job_start(job)) {
        g_lists[src].state = MODEL_LIST_FETCHING;
    } else {
        g_lists[src].state = MODEL_LIST_FAILED;
    }
}

const ModelList *model_catalog_agent(const char *agent_id) {
    if (!agent_id) return NULL;
    catalog_cache_load();
    if (strcmp(agent_id, "claude") == 0)   return &g_lists[SRC_CLAUDE];
    if (strcmp(agent_id, "opencode") == 0) return &g_lists[SRC_OPENCODE];
    if (strcmp(agent_id, "codex") == 0)    return &g_lists[SRC_CODEX];
    if (strcmp(agent_id, "grok") == 0)     return &g_lists[SRC_GROK];
    return NULL;
}

const ModelList *model_catalog_api(const char *provider) {
    if (!provider) return NULL;
    catalog_cache_load();
    if (strcmp(provider, "anthropic") == 0)  return &g_lists[SRC_API_ANTHROPIC];
    if (strcmp(provider, "openai") == 0)     return &g_lists[SRC_API_OPENAI];
    if (strcmp(provider, "openrouter") == 0) return &g_lists[SRC_API_OPENROUTER];
    if (strcmp(provider, "custom") == 0)     return &g_lists[SRC_API_CUSTOM];
    return NULL;
}
