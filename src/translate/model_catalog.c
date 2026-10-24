/*
 * Liu - Translate model catalog (see model_catalog.h for the contract).
 *
 * Fetch children mirror translate_agent_spawn's fork+exec shape: argv-only
 * exec (no shell), CLOEXEC pipe ends, non-blocking parent read, WNOHANG
 * reap from the tick. Secrets ride a `curl -K -` stdin config pumped by a
 * grandchild — an API key never appears in `ps` output.
 */
#include "translate/model_catalog.h"
#include "core/curl_path.h"
#include "core/api_url.h"

#include "core/agent_detect.h"
#include "core/atomic_file.h"
#include "core/checked.h"
#include "cJSON.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <limits.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifndef PLATFORM_WIN32
#include <sys/wait.h>
#endif

extern char **environ;

/* ----------------------------------------------------------------- *
 *  Source table                                                      *
 * ----------------------------------------------------------------- */

enum {
    SRC_CLAUDE = 0,      /* models.dev: anthropic            */
    SRC_OPENCODE,        /* models.dev: opencode-go          */
    SRC_CODEX,           /* models.dev: openai               */
    SRC_GROK,            /* `grok models`                    */
    SRC_ANTIGRAVITY,     /* `agy models`                     */
    SRC_API_ANTHROPIC,   /* GET api.anthropic.com/v1/models  */
    SRC_API_OPENAI,      /* GET api.openai.com/v1/models     */
    SRC_API_OPENROUTER,  /* GET openrouter.ai/api/v1/models  */
    SRC_API_CUSTOM,      /* GET <base>/models                */
    SRC_COUNT,
};

/* Job kinds — one curl/agent-CLI child can fill several sources. */
enum {
    JOB_MODELSDEV = 0,   /* fills CLAUDE + OPENCODE + CODEX  */
    JOB_GROK,
    JOB_ANTIGRAVITY,
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
/* The fingerprint an API fetch STARTED under, per job kind. A result is
 * published only while it is still the current one for its source. */
static u32       g_job_fp[JOB_COUNT];

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
    case SRC_ANTIGRAVITY: return "antigravity";
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
    if (!out || cap == 0) return;
    usize o = 0;
    for (const char *p = s; p && *p && o < cap - 1u; p++) {
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
    int n = snprintf(out, cap, "%s/.config/Liu/agent_models.json", home);
    return n > 0 && (usize)n < cap;
}

static void catalog_cache_load(void) {
    if (g_cache_loaded) return;
    g_cache_loaded = true;

    char path[1024];
    if (!catalog_cache_path(path, sizeof path)) return;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return; }
    long sz = ftell(f);
    if (sz <= 0 || sz > 1024 * 1024) { fclose(f); return; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return; }
    char *body = (char *)malloc((usize)sz + 1);
    if (!body) { fclose(f); return; }
    usize rd = fread(body, 1, (usize)sz, f);
    fclose(f);
    if (rd != (usize)sz) { free(body); return; }
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
        if (!cJSON_IsNumber(ts) || !isfinite(ts->valuedouble) ||
            ts->valuedouble < 0 || ts->valuedouble > 4102444800.0 ||
            !cJSON_IsArray(ids)) continue;
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

    AtomicFileWriter writer;
    if (atomic_file_begin_private(&writer, path)) {
        usize len = strlen(text);
        if (fwrite(text, 1, len, writer.stream) != len)
            atomic_file_abort(&writer);
        else
            (void)atomic_file_commit(&writer);
    }
    free(text);
}

/* ----------------------------------------------------------------- *
 *  Spawn helpers                                                     *
 * ----------------------------------------------------------------- */

static bool catalog_write_all(int fd, const char *data, usize len) {
    usize off = 0;
    while (off < len) {
        ssize_t n = write(fd, data + off, len - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        off += (usize)n;
    }
    return true;
}

static int catalog_fd_above_stdio(int fd) {
    if (fd < 0) return fd;
    if (fd < 3) {
        int moved = fcntl(fd, F_DUPFD_CLOEXEC, 3);
        close(fd);
        return moved;
    }
    int flags = fcntl(fd, F_GETFD, 0);
    if (flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Spawn `argv`, optionally feeding stdin from an unlinked private file.
 * stderr goes to /dev/null: these children's stdout is machine-parsed. */
static bool catalog_spawn(char *const argv[], const char *stdin_body,
                          pid_t *out_pid, int *out_fd) {
    if (!argv || !argv[0] || !out_pid || !out_fd) return false;
    int pipefd[2];
    if (pipe(pipefd) != 0) return false;
    pipefd[0] = catalog_fd_above_stdio(pipefd[0]);
    pipefd[1] = catalog_fd_above_stdio(pipefd[1]);
    if (pipefd[0] < 0 || pipefd[1] < 0) {
        if (pipefd[0] >= 0) close(pipefd[0]);
        if (pipefd[1] >= 0) close(pipefd[1]);
        return false;
    }
    int stdin_fd = -1;
    if (stdin_body && stdin_body[0]) {
        char tmp[] = "/tmp/liu-catalog-input-XXXXXX";
        stdin_fd = mkstemp(tmp);
        if (stdin_fd >= 0) (void)unlink(tmp);
        stdin_fd = catalog_fd_above_stdio(stdin_fd);
        if (stdin_fd < 0 ||
            !catalog_write_all(stdin_fd, stdin_body, strlen(stdin_body)) ||
            lseek(stdin_fd, 0, SEEK_SET) != 0) {
            if (stdin_fd >= 0) close(stdin_fd);
            close(pipefd[0]); close(pipefd[1]);
            return false;
        }
    }

    posix_spawn_file_actions_t fa;
    int rc = posix_spawn_file_actions_init(&fa);
    bool fa_inited = rc == 0;
    if (rc == 0 && stdin_fd >= 0)
        rc = posix_spawn_file_actions_adddup2(&fa, stdin_fd, STDIN_FILENO);
    else if (rc == 0)
        rc = posix_spawn_file_actions_addopen(&fa, STDIN_FILENO,
                                               "/dev/null", O_RDONLY, 0);
    if (rc == 0) rc = posix_spawn_file_actions_adddup2(&fa, pipefd[1], STDOUT_FILENO);
    if (rc == 0) rc = posix_spawn_file_actions_addopen(&fa, STDERR_FILENO,
                                                        "/dev/null", O_WRONLY, 0);
    if (rc == 0 && stdin_fd >= 0)
        rc = posix_spawn_file_actions_addclose(&fa, stdin_fd);
    if (rc == 0) rc = posix_spawn_file_actions_addclose(&fa, pipefd[0]);
    if (rc == 0) rc = posix_spawn_file_actions_addclose(&fa, pipefd[1]);

    pid_t pid = 0;
    if (rc == 0) rc = posix_spawnp(&pid, strcmp(argv[0], "curl") == 0 ? liu_curl_exe() : argv[0],
                                    &fa, NULL, argv, environ);
    if (fa_inited) posix_spawn_file_actions_destroy(&fa);
    if (stdin_fd >= 0) close(stdin_fd);
    close(pipefd[1]);
    if (rc != 0 || pid <= 0) {
        close(pipefd[0]);
        return false;
    }

    int flags = fcntl(pipefd[0], F_GETFL, 0);
    if (flags < 0 || fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK) != 0) {
        close(pipefd[0]);
        (void)kill(pid, SIGKILL);
        while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
        return false;
    }
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
    if (!out || cap == 0) return;
    usize off = 0;
    i32 w = snprintf(out + off, cap - off,
                     "url = \"%s\"\n" LIU_API_CURL_PROTO_LINES
                     "silent\nmax-time = 25\nlocation\n", url);
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
    case JOB_GROK:
    case JOB_ANTIGRAVITY: {
        /* Both CLIs list their models with a bare `models` subcommand. Resolve
         * the binary the same way the translate spawn does — PATH plus the
         * curated GUI-launch fallback dirs. */
        const char *want = kind == JOB_GROK ? "grok" : "antigravity";
        AgentInfo agents[AGENT_MAX] = {0};
        i32 n = agent_detect_available(agents, AGENT_MAX);
        const char *bin = NULL;
        for (i32 i = 0; i < n; i++) {
            if (strcmp(agents[i].id, want) == 0) {
                bin = agents[i].path[0] ? agents[i].path : agents[i].binary;
                break;
            }
        }
        if (!bin) return false;
        char *argv[] = {(char *)bin, (char *)"models", NULL};
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

/* `agy models` → one model per line, no bullets and no header. A line is the
 * bare id, or (agy 1.3.4) the id followed by a TAB and the display name:
 *     gemini-3.8-flash-low<TAB>Gemini 3.8 Flash (Low)
 *     claude-sonnet-4-6<TAB>Claude Sonnet 4.6 (Thinking)
 * The id is the first column. A line counts only when its first token is the
 * whole trimmed line or ends at a TAB, so an "Available models:" style banner
 * (words separated by spaces) is skipped rather than harvested as a model
 * called "Available". */
static void parse_antigravity(const char *body) {
    ModelEntry entries[32];
    i32 n = 0;
    const char *p = body;
    while (p && *p && n < (i32)(sizeof(entries) / sizeof(entries[0]))) {
        const char *eol = strchr(p, '\n');
        const char *end = eol ? eol : p + strlen(p);
        const char *s = p;
        while (s < end && (unsigned char)*s <= ' ') s++;
        const char *tok = s;
        while (s < end && (unsigned char)*s > ' ') s++;
        usize toklen = (usize)(s - tok);
        bool column = s < end && *s == '\t';
        const char *rest = s;
        while (rest < end && (unsigned char)*rest <= ' ') rest++;
        if (toklen > 0 && toklen < MODEL_ID_CAP && (rest == end || column) &&
            tok[toklen - 1] != ':') {
            ModelEntry *e = &entries[n++];
            memcpy(e->id, tok, toklen);
            e->id[toklen] = '\0';
            e->sort_key[0] = '\0';
        }
        p = eol ? eol + 1 : NULL;
    }
    list_commit(&g_lists[SRC_ANTIGRAVITY], entries, n, false);
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
    case JOB_ANTIGRAVITY:
        if (exited_ok) parse_antigravity(body);
        else g_lists[SRC_ANTIGRAVITY].state = MODEL_LIST_FAILED;
        break;
    case JOB_API_ANTHROPIC:
    case JOB_API_OPENAI:
    case JOB_API_OPENROUTER:
    case JOB_API_CUSTOM: {
        i32 src = job->kind == JOB_API_ANTHROPIC  ? SRC_API_ANTHROPIC
                : job->kind == JOB_API_OPENAI     ? SRC_API_OPENAI
                : job->kind == JOB_API_OPENROUTER ? SRC_API_OPENROUTER
                                                  : SRC_API_CUSTOM;
        /* An answer for settings that are no longer the current ones belongs
         * to nobody: not READY (it would show the old account's models as the
         * new key's, fresh, for the whole TTL), not FAILED (that is the new
         * settings' state to report). Drop it and open the retry gate so the
         * current settings are fetched at once. */
        if (g_job_fp[job->kind] != g_api_fingerprint[src]) {
            g_last_attempt[job->kind] = 0;
            break;
        }
        if (exited_ok) parse_api_models(body, src);
        else g_lists[src].state = MODEL_LIST_FAILED;
        break;
    }
    default:
        break;
    }
}

static bool catalog_job_append(CatalogJob *job, const char *data, usize len) {
    if (!job || (!data && len > 0) || job->len > job->cap) return false;
    usize required;
    if (!checked_add_usize(job->len, len, &required) ||
        !checked_add_usize(required, 1u, &required) ||
        required > CATALOG_BUF_HARD_CAP) return false;
    if (required > job->cap) {
        usize want = job->cap ? job->cap : 64u * 1024u;
        while (want < required) {
            if (want > CATALOG_BUF_HARD_CAP / 2u) {
                want = CATALOG_BUF_HARD_CAP;
                break;
            }
            want *= 2u;
        }
        char *grown = realloc(job->buf, want);
        if (!grown) return false;
        job->buf = grown;
        job->cap = want;
    }
    if (len) memcpy(job->buf + job->len, data, len);
    job->len += len;
    job->buf[job->len] = '\0';
    return true;
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
                if (!catalog_job_append(job, chunk, (usize)n)) {
                    /* Body exceeds the hard cap or allocation failed. */
                    kill(job->pid, SIGKILL);
                    break;
                }
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
                if (!catalog_job_append(job, chunk, (usize)n)) break;
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

    /* Agents that list their own models through a subprocess rather than
     * models.dev. */
    i32 cli_src = -1, cli_job = -1;
    if (strcmp(agent_id, "grok") == 0) {
        cli_src = SRC_GROK; cli_job = JOB_GROK;
    } else if (strcmp(agent_id, "antigravity") == 0) {
        cli_src = SRC_ANTIGRAVITY; cli_job = JOB_ANTIGRAVITY;
    }
    if (cli_src >= 0) {
        if (!catalog_should_fetch(&g_lists[cli_src], cli_job,
                                  CATALOG_TTL_AGENT_SEC)) return;
        g_last_attempt[cli_job] = (long long)time(NULL);
        g_lists[cli_src].state = catalog_job_start(cli_job)
            ? MODEL_LIST_FETCHING : MODEL_LIST_FAILED;
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

    /* Key/base change invalidates whatever was fetched before — including a
     * fetch still running for the old settings, which is ended here so its
     * slot frees (and whose answer, should it land anyway, is dropped by the
     * fingerprint check in catalog_job_finish). */
    u32 fp = catalog_hash(api_key ? api_key : "", base_url ? base_url : "");
    if (fp != g_api_fingerprint[src]) {
        g_api_fingerprint[src] = fp;
        g_lists[src].state = MODEL_LIST_IDLE;
        g_lists[src].count = 0;
        g_lists[src].fetched_at = 0;
        g_last_attempt[job] = 0;
        for (usize i = 0; i < sizeof(g_jobs) / sizeof(g_jobs[0]); i++) {
            if (g_jobs[i].kind == job && g_jobs[i].pid > 0 && g_job_fp[job] != fp)
                kill(g_jobs[i].pid, SIGKILL);       /* reaped by the next tick */
        }
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
        /* http(s) only; an endpoint of any other shape fetches nothing
         * (the spawn refuses an empty URL). */
        if (liu_api_base_url_ok(base_san))
            snprintf(g_custom_models_url, sizeof g_custom_models_url,
                     "%.*s/models", (int)blen, base_san);
        else
            g_custom_models_url[0] = '\0';
    }

    g_last_attempt[job] = (long long)time(NULL);
    g_job_fp[job] = fp;                 /* what this fetch is FOR */
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
    if (strcmp(agent_id, "antigravity") == 0) return &g_lists[SRC_ANTIGRAVITY];
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
