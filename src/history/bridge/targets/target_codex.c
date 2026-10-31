/*
 * This adapter uses Codex app-server over stdio JSON-RPC, two ways.
 *
 * First, Codex's own session import (externalAgentConfig/detect + /import):
 * Codex reads a Claude Code transcript and writes a thread whose rollout
 * carries the turns as Codex shows them — the messages are on screen when
 * `codex resume` opens it, as in any Codex chat. That importer only takes the
 * sessions its detection finds under $HOME/.claude/projects, so the session is
 * staged in a private folder that serves as the app-server's HOME (the
 * process's own CODEX_HOME is passed explicitly, so the thread lands in the
 * user's store): a Claude Code session as a byte copy of its file, any other
 * agent's as a Claude Code transcript written from the IR. A fresh folder per
 * migration is a path Codex has not imported before, so every migration is a
 * new thread. The folder is removed afterwards.
 *
 * The import keeps tool calls as text notes; the thread's display records are
 * then rewritten so they show as Codex's own cells (codex_display.h). Codex's
 * reader is asked whether it finds what the rewrite put there, and the rollout
 * goes back as the import wrote it when it does not.
 *
 * Second, when this Codex has no such import or does not take the session:
 * thread/start creates a thread from cwd and thread/inject_items appends
 * model-visible Responses API items. The model has the history; Codex's
 * screen shows none of it. The returned thread ID is used with
 * codex resume <threadId>.
 *
 * The bridge maps user text to message items with role user and input_text
 * content, assistant text to role assistant and output_text content, context
 * to a role developer message with input_text content, tool calls to
 * function_call items, and tool results to function_call_output items.
 *
 * Function-call arguments must be serialized as a JSON string. Each
 * function_call_output must follow a function_call with the same call_id.
 * Orphan outputs are dropped and counted because a model turn cannot consume a
 * result for a call it cannot see.
 *
 * Injected items become part of the thread's stored history. Requests are
 * written directly to the child process stdin without a shell, so session IDs
 * and paths are not interpolated into a command.
 */
#include "history/bridge/targets.h"
#include "history/bridge/targets/codex_display.h"
#include "history/bridge/compatibility.h"
#include "history/bridge/jsonw.h"
#include "history/bridge/tool_map.h"
#include "history/bridge/writers.h"
#include "history/util.h"

#include "cJSON.h"

#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* app-server startup pays for MCP server launches and a config read, which
 * takes seconds with several MCP servers configured. 90 s is "something is
 * wrong", not "this is slow". */
#define CODEX_TIMEOUT_SEC 90
#define CODEX_REPLY_CAP   ((usize)MB(2))
/* Codex's import reports its end in a notification after the request's own
 * reply; converting a long transcript takes a few seconds at most. */
#define CODEX_IMPORT_WAIT_SEC 120
/* A Claude Code transcript larger than this is staged from the IR (which is
 * bounded) rather than copied whole. */
#define CODEX_STAGE_COPY_MAX ((i64)MB(256))
/* The rollout an import wrote is read whole to draw its tool calls as cells
 * (codex_display.h); a larger one is left as it is. */
#define CODEX_DISPLAY_MAX ((i64)MB(256))
/* Turns asked for at a time when the rewritten thread is checked. One: a
 * reply is one protocol line, and a turn can be long. */
#define CODEX_CHECK_PAGE 1

static bool codex_available(void) {
    return bridge_binary_available("codex");
}

static bool codex_full_history(void) {
    /* inject_items takes raw Responses items, which is exactly the shape tool
     * calls and outputs live in — so yes, and not conditionally. */
    return true;
}

/* Grow-on-demand byte buffer for the request stream. */
typedef struct { char *p; usize len, cap; bool bad; } Buf;

static void buf_put(Buf *b, const char *s, usize n) {
    if (b->bad || n == 0) return;
    if (b->len + n + 1 > b->cap) {
        usize ncap = b->cap ? b->cap * 2 : KB(64);
        while (ncap < b->len + n + 1) {
            if (ncap > (usize)MB(64)) { b->bad = true; return; }
            ncap *= 2;
        }
        char *q = realloc(b->p, ncap);
        if (!q) { b->bad = true; return; }
        b->p = q; b->cap = ncap;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
}

static void buf_puts(Buf *b, const char *s) { buf_put(b, s, strlen(s)); }

/* Use open_memstream to serialize JSON into memory; it is POSIX 2008 and
 * available on both supported platforms. */
typedef struct { FILE *f; char *base; size_t size; JsonW w; } MemW;

static bool memw_begin(MemW *m) {
    memset(m, 0, sizeof *m);
    m->f = open_memstream(&m->base, &m->size);
    if (!m->f) return false;
    jsonw_open_stream(&m->w, m->f);
    return true;
}

static bool memw_end(MemW *m, Buf *out) {
    bool ok = jsonw_ok(&m->w);
    jsonw_close(&m->w, false);
    if (fclose(m->f) != 0) ok = false;
    m->f = NULL;
    if (ok && m->base) {
        buf_put(out, m->base, m->size);
        buf_puts(out, "\n");
    }
    free(m->base);
    m->base = NULL;
    return ok;
}

/* One Responses item for `e`, or false when the event has no representation
 * (and is therefore not an error, just not an item). */
static bool write_item(JsonW *w, const BridgeHistory *h, const BridgeEvent *e,
                       const char *open_call_id) {
    switch (e->type) {
        case BRIDGE_EVENT_USER:
        case BRIDGE_EVENT_ASSISTANT:
        case BRIDGE_EVENT_CONTEXT:
        case BRIDGE_EVENT_COMPACTION: {
            if (!e->text || !e->text[0]) return false;
            bool asst = (e->type == BRIDGE_EVENT_ASSISTANT);
            const char *role = asst ? "assistant"
                             : (e->type == BRIDGE_EVENT_USER ? "user" : "developer");
            jsonw_obj_begin(w, NULL);
            jsonw_str(w, "type", "message");
            jsonw_str(w, "role", role);
            jsonw_arr_begin(w, "content");
            jsonw_obj_begin(w, NULL);
            /* input_text vs output_text is not cosmetic: the Responses API
             * rejects an assistant message carrying input_text. */
            jsonw_str(w, "type", asst ? "output_text" : "input_text");
            if (e->type == BRIDGE_EVENT_COMPACTION) {
                /* Label a compaction summary so the model can tell a summary
                 * of dropped history from history. */
                jsonw_obj_end(w);
                jsonw_obj_begin(w, NULL);
                jsonw_str(w, "type", "input_text");
                char lbl[256];
                snprintf(lbl, sizeof lbl,
                         "[Summary of earlier %s conversation]\n",
                         chat_tool_name(h->source_tool));
                jsonw_str(w, "text", lbl);
                jsonw_obj_end(w);
                jsonw_obj_begin(w, NULL);
                jsonw_str(w, "type", "input_text");
            }
            jsonw_str(w, "text", e->text);
            jsonw_obj_end(w);
            jsonw_arr_end(w);
            jsonw_obj_end(w);
            return true;
        }

        case BRIDGE_EVENT_TOOL_CALL: {
            const char *name = bridge_tool_name_for(CHAT_TOOL_CODEX, e->tool_kind);
            if (!name) name = e->tool_name;
            if (!name || !name[0]) return false;
            jsonw_obj_begin(w, NULL);
            jsonw_str(w, "type", "function_call");
            jsonw_str(w, "name", name);
            jsonw_str(w, "call_id", e->tool_call_id);
            /* A JSON *string*, always — see the header. An input that is not
             * valid JSON is wrapped so the field still parses as one. */
            if (e->tool_input && jsonw_is_json(e->tool_input)) {
                jsonw_str(w, "arguments", e->tool_input);
            } else if (e->tool_input && e->tool_input[0]) {
                MemW inner;
                if (memw_begin(&inner)) {
                    jsonw_obj_begin(&inner.w, NULL);
                    jsonw_str(&inner.w, "input", e->tool_input);
                    jsonw_obj_end(&inner.w);
                    jsonw_close(&inner.w, false);
                    fflush(inner.f);
                    fclose(inner.f);
                    inner.f = NULL;
                    jsonw_str(w, "arguments", inner.base ? inner.base : "{}");
                    free(inner.base);
                } else {
                    jsonw_str(w, "arguments", "{}");
                }
            } else {
                jsonw_str(w, "arguments", "{}");
            }
            jsonw_obj_end(w);
            return true;
        }

        case BRIDGE_EVENT_TOOL_RESULT: {
            /* Orphan outputs are dropped by the caller; at this point
             * there is a matching call. */
            jsonw_obj_begin(w, NULL);
            jsonw_str(w, "type", "function_call_output");
            jsonw_str(w, "call_id", open_call_id);
            jsonw_str(w, "output", e->tool_output ? e->tool_output : "");
            jsonw_obj_end(w);
            return true;
        }

        default:
            return false;
    }
}

/* ---------------------------------------------------------- app-server --- */

/* One JSON-RPC exchange over a LIVE app-server. Sends `req` (already
 * newline-terminated) and returns the response line whose id matches, or NULL.
 *
 * Written against a persistent BridgeProc rather than a one-shot pipe because
 * the app-server treats stdin EOF as a client disconnect: on 0.149.1, piping
 * the whole request stream in and closing produces NO output at all and the
 * process exits before servicing a single method. Interleaving is also the
 * only way to address a thread by the id the previous response returned. */
typedef enum {
    CODEX_REPLY_RESULT = 0,   /* a result object came back                    */
    CODEX_REPLY_ERROR,        /* the server answered with an error            */
    CODEX_REPLY_NONE,         /* no answer: dead pipe, timeout, garbage       */
} CodexReply;

static cJSON *codex_call_ex(BridgeProc *p, const char *req, i64 id, i32 timeout_sec,
                            CodexReply *how) {
    *how = CODEX_REPLY_NONE;
    if (!bridge_proc_send(p, req, strlen(req))) return NULL;
    char needle[32];
    snprintf(needle, sizeof needle, "\"id\":%lld", (long long)id);
    i64 deadline = bridge_now_ms() + (i64)timeout_sec * 1000;
    for (;;) {
        i64 left_ms = deadline - bridge_now_ms();
        if (left_ms <= 0) return NULL;
        char *line = bridge_proc_await_line(p, needle, (i32)((left_ms + 999) / 1000));
        if (!line) return NULL;

        cJSON *o = cJSON_Parse(line);
        free(line);
        if (!o) continue;
        /* The substring test can match an id inside a notification's payload,
         * so the parsed object is checked properly before it is believed. */
        i64 got = 0;
        if (!hist_cjson_i64(o, "id", &got) || got != id) { cJSON_Delete(o); continue; }
        cJSON *res = cJSON_GetObjectItemCaseSensitive(o, "result");
        if (!res) {
            *how = cJSON_GetObjectItemCaseSensitive(o, "error") ? CODEX_REPLY_ERROR
                                                                 : CODEX_REPLY_NONE;
            cJSON_Delete(o);
            return NULL;
        }
        cJSON *dup = cJSON_Duplicate(res, 1);
        cJSON_Delete(o);
        *how = dup ? CODEX_REPLY_RESULT : CODEX_REPLY_NONE;
        return dup;
    }
}

static cJSON *codex_call(BridgeProc *p, const char *req, i64 id, i32 timeout_sec) {
    CodexReply how;
    return codex_call_ex(p, req, id, timeout_sec, &how);
}

/* initialize + initialized on a freshly started app-server. */
static bool codex_handshake(BridgeProc *p) {
    Buf req = {0};
    MemW m;
    if (!memw_begin(&m)) return false;
    jsonw_obj_begin(&m.w, NULL);
    jsonw_str(&m.w, "jsonrpc", "2.0");
    jsonw_i64(&m.w, "id", 1);
    jsonw_str(&m.w, "method", "initialize");
    jsonw_obj_begin(&m.w, "params");
    jsonw_obj_begin(&m.w, "clientInfo");
    jsonw_str(&m.w, "name", "liu");
    jsonw_str(&m.w, "version", "1");
    jsonw_str(&m.w, "title", "Liu");
    jsonw_obj_end(&m.w);
    jsonw_obj_end(&m.w);
    jsonw_obj_end(&m.w);
    memw_end(&m, &req);
    if (req.bad) { free(req.p); return false; }

    cJSON *res = codex_call(p, req.p, 1, CODEX_TIMEOUT_SEC);
    free(req.p);
    if (!res) return false;
    cJSON_Delete(res);

    /* The server holds thread/start until it has seen this notification, and
     * a notification has no id so there is nothing to await. */
    static const char kInitialized[] =
        "{\"jsonrpc\":\"2.0\",\"method\":\"initialized\",\"params\":{}}\n";
    return bridge_proc_send(p, kInitialized, sizeof kInitialized - 1);
}

/* The model-visible history alone (see the header): the way in for a Codex
 * whose own import is missing or did not take the session. */
static bool codex_import_inject(const BridgeHistory *h, BridgeImportResult *r) {
    const char *cwd = h->cwd;
    const char *argv[] = { "codex", "app-server", NULL };
    BridgeProc *p = bridge_proc_start(argv);
    if (!p) {
        snprintf(r->error, sizeof r->error, "Could not start codex app-server.");
        return false;
    }

    bool ok = false;
    u32  dropped = 0;
    Buf  req = {0};

    if (!codex_handshake(p)) {
        snprintf(r->error, sizeof r->error, "Codex app-server did not initialize.");
        goto done;
    }

    /* thread/start */
    {
        MemW m;
        if (!memw_begin(&m)) goto done;
        jsonw_obj_begin(&m.w, NULL);
        jsonw_str(&m.w, "jsonrpc", "2.0");
        jsonw_i64(&m.w, "id", 2);
        jsonw_str(&m.w, "method", "thread/start");
        jsonw_obj_begin(&m.w, "params");
        jsonw_str(&m.w, "cwd", cwd);
        jsonw_obj_end(&m.w);
        jsonw_obj_end(&m.w);
        memw_end(&m, &req);
        if (req.bad) {
            snprintf(r->error, sizeof r->error, "Out of memory building the request.");
            goto done;
        }

        cJSON *res = codex_call(p, req.p, 2, CODEX_TIMEOUT_SEC);
        free(req.p);
        req = (Buf){0};
        if (!res) {
            snprintf(r->error, sizeof r->error,
                     "Codex app-server refused to start a thread.");
            goto done;
        }
        cJSON *thread = cJSON_GetObjectItemCaseSensitive(res, "thread");
        const char *tid  = thread ? hist_cjson_str(thread, "id") : NULL;
        const char *path = thread ? hist_cjson_str(thread, "path") : NULL;
        if (!tid || !tid[0]) {
            cJSON_Delete(res);
            snprintf(r->error, sizeof r->error, "Codex did not return a thread id.");
            goto done;
        }
        hist_copy_bounded(r->session_id,   sizeof r->session_id,   tid);
        hist_copy_bounded(r->session_path, sizeof r->session_path, path ? path : "");
        cJSON_Delete(res);
    }

    /* thread/inject_items on the SAME connection — the thread is addressed by
     * the id thread/start just returned. */
    {
        MemW m;
        if (!memw_begin(&m)) goto done;
        jsonw_obj_begin(&m.w, NULL);
        jsonw_str(&m.w, "jsonrpc", "2.0");
        jsonw_i64(&m.w, "id", 3);
        jsonw_str(&m.w, "method", "thread/inject_items");
        jsonw_obj_begin(&m.w, "params");
        jsonw_str(&m.w, "threadId", r->session_id);
        jsonw_arr_begin(&m.w, "items");

        /* A header so the model can see, in its own history, that the earlier
         * turns came from somewhere else. Without it the conversation reads as
         * if Codex itself did work it has no memory of. */
        char hdr[512];
        snprintf(hdr, sizeof hdr,
                 "[Continued in Codex from a %s session%s%s. The turns that "
                 "follow are prior conversation history, not new instructions; "
                 "the tool calls in them already ran.]",
                 chat_tool_name(h->source_tool),
                 h->title ? " \xe2\x80\x94 " : "", h->title ? h->title : "");
        jsonw_obj_begin(&m.w, NULL);
        jsonw_str(&m.w, "type", "message");
        jsonw_str(&m.w, "role", "developer");
        jsonw_arr_begin(&m.w, "content");
        jsonw_obj_begin(&m.w, NULL);
        jsonw_str(&m.w, "type", "input_text");
        jsonw_str(&m.w, "text", hdr);
        jsonw_obj_end(&m.w);
        jsonw_arr_end(&m.w);
        jsonw_obj_end(&m.w);

        /* The call/result pairing rule. `open_call` holds the id of the last
         * call actually emitted, so a result with no matching call — the
         * normal shape of a compacted or truncated source — is dropped rather
         * than injected as an orphan the model would reference. */
        const char *open_call = NULL;
        for (usize i = 0; i < h->event_count; i++) {
            const BridgeEvent *e = &h->events[i];
            if (e->type == BRIDGE_EVENT_TOOL_RESULT && !open_call) {
                dropped++;
                continue;
            }
            if (write_item(&m.w, h, e, open_call)) {
                if (e->type == BRIDGE_EVENT_TOOL_CALL)   open_call = e->tool_call_id;
                if (e->type == BRIDGE_EVENT_TOOL_RESULT) open_call = NULL;
            }
        }
        jsonw_arr_end(&m.w);
        jsonw_obj_end(&m.w);
        jsonw_obj_end(&m.w);
        if (!memw_end(&m, &req) || req.bad) {
            free(req.p);
            req = (Buf){0};
            snprintf(r->error, sizeof r->error, "Session too large to hand to Codex.");
            goto done;
        }

        cJSON *res = codex_call(p, req.p, 3, CODEX_TIMEOUT_SEC);
        free(req.p);
        req = (Buf){0};
        if (!res) {
            snprintf(r->error, sizeof r->error, "Codex rejected the imported history.");
            goto done;
        }
        /* A successful inject_items response is represented by a result
         * object, which may be empty. */
        cJSON_Delete(res);
    }

    /* Read the thread back on the SAME connection while it is still cheap:
     * the validator would otherwise pay another ~4 s app-server start. */
    {
        MemW m;
        if (!memw_begin(&m)) goto done;
        jsonw_obj_begin(&m.w, NULL);
        jsonw_str(&m.w, "jsonrpc", "2.0");
        jsonw_i64(&m.w, "id", 4);
        jsonw_str(&m.w, "method", "thread/read");
        jsonw_obj_begin(&m.w, "params");
        jsonw_str(&m.w, "threadId", r->session_id);
        jsonw_obj_end(&m.w);
        jsonw_obj_end(&m.w);
        memw_end(&m, &req);
        if (!req.bad) {
            cJSON *res = codex_call(p, req.p, 4, CODEX_TIMEOUT_SEC);
            if (res) {
                cJSON *thread = cJSON_GetObjectItemCaseSensitive(res, "thread");
                const char *id = thread ? hist_cjson_str(thread, "id") : NULL;
                ok = id && strcmp(id, r->session_id) == 0;
                cJSON_Delete(res);
            }
        }
        free(req.p);
        req = (Buf){0};
        if (!ok) {
            snprintf(r->error, sizeof r->error,
                     "Codex did not confirm the imported thread.");
            goto done;
        }
    }

    r->warnings += dropped;
    snprintf(r->launch_cmd, sizeof r->launch_cmd, "codex resume %s", r->session_id);

done:
    free(req.p);
    bridge_proc_stop(p);
    return ok;
}

/* ------------------------------------------------- Codex's own import --- */

/* The private HOME a session is staged in for Codex's import. */
typedef struct {
    char root[BRIDGE_PATH_CAP];   /* <state>/codex-import-XXXXXX, removed after */
    char file[BRIDGE_PATH_CAP];   /* <root>/.claude/projects/<slug>/<name>      */
    char name[64];                /* <uuid>.jsonl: how detection names the file */
} CodexStage;

/* The Codex home this process's `codex` uses: $CODEX_HOME, else ~/.codex. */
static bool codex_home_dir(char *out, usize cap) {
    const char *env = hist_env_dir("CODEX_HOME");
    if (env) {
        int n = snprintf(out, cap, "%s", env);
        return env[0] == '/' && n > 0 && (usize)n < cap;
    }
    return hist_home_path(out, cap, ".codex");
}

/* Copy the regular file `src` to the new file `dst`, 0600. False on anything
 * else at `src` (a link, a FIFO, a file past the size limit), with `dst`
 * removed. */
static bool codex_copy_transcript(const char *src, const char *dst) {
    int in = open(src, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (in < 0) return false;
    struct stat st;
    if (fstat(in, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
        (i64)st.st_size > CODEX_STAGE_COPY_MAX) {
        close(in);
        return false;
    }
    int out = open(dst, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (out < 0) { close(in); return false; }
    char buf[64 * 1024];
    bool ok = true;
    for (;;) {
        ssize_t n = read(in, buf, sizeof buf);
        if (n == 0) break;
        if (n < 0) {
            if (errno == EINTR) continue;
            ok = false;
            break;
        }
        for (ssize_t off = 0; off < n;) {
            ssize_t w = write(out, buf + off, (usize)(n - off));
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) { ok = false; break; }
            off += w;
        }
        if (!ok) break;
    }
    close(in);
    if (close(out) != 0) ok = false;
    if (!ok) unlink(dst);
    return ok;
}

static int codex_stage_remove_one(const char *path, const struct stat *st, int flag,
                                  struct FTW *ftw) {
    (void)st; (void)flag; (void)ftw;
    remove(path);
    return 0;
}

/* Remove the staging folder and everything in it, links as links. Only a
 * folder this file made: a `codex-import-` folder in Liu's bridge state. */
static void codex_stage_remove(CodexStage *st) {
    if (!st->root[0] || !strstr(st->root, "/codex-import-")) return;
    nftw(st->root, codex_stage_remove_one, 16, FTW_DEPTH | FTW_PHYS);
    st->root[0] = '\0';
}

/* Stage `h` for Codex's import (see the header). False when nothing could be
 * staged; the folder is gone then. */
static bool codex_stage(const BridgeHistory *h, CodexStage *st, u32 *warnings) {
    memset(st, 0, sizeof *st);
    char state[BRIDGE_PATH_CAP], slug[BRIDGE_PATH_CAP], uuid[64], dir[BRIDGE_PATH_CAP];
    if (!bridge_state_dir(state, sizeof state)) return false;
    int n = snprintf(st->root, sizeof st->root, "%s/codex-import-XXXXXX", state);
    if (n <= 0 || (usize)n >= sizeof st->root || !mkdtemp(st->root)) {
        st->root[0] = '\0';
        return false;
    }
    if (!bridge_claude_project_slug(h->cwd, slug, sizeof slug) ||
        !bridge_new_uuid(uuid, sizeof uuid)) {
        codex_stage_remove(st);
        return false;
    }
    n = snprintf(dir, sizeof dir, "%s/.claude/projects/%s", st->root, slug);
    int m = snprintf(st->name, sizeof st->name, "%s.jsonl", uuid);
    int k = (n > 0 && (usize)n < sizeof dir)
          ? snprintf(st->file, sizeof st->file, "%s/%s", dir, st->name) : -1;
    if (n <= 0 || (usize)n >= sizeof dir || m <= 0 || (usize)m >= sizeof st->name ||
        k <= 0 || (usize)k >= sizeof st->file || !bridge_mkdir_p(dir)) {
        codex_stage_remove(st);
        return false;
    }

    /* A Claude Code session goes as it is: Codex's reader of that format sees
     * everything the file holds. Anything else, or a copy that failed, is
     * written from the IR. */
    bool staged = h->source_tool == CHAT_TOOL_CLAUDE && h->source_path &&
                  h->source_path[0] == '/' &&
                  codex_copy_transcript(h->source_path, st->file);
    if (!staged) {
        u32 w = 0;
        staged = bridge_write_claude_session(h, uuid, st->file, false, &w);
        if (staged && warnings) *warnings += w;
    }
    if (!staged) codex_stage_remove(st);
    return staged;
}

typedef enum {
    CODEX_OWN_DONE = 0,       /* the thread exists; `r` names it              */
    CODEX_OWN_UNAVAILABLE,    /* nothing was created: take the other way      */
    CODEX_OWN_FAILED,         /* stop; `r->error` says why                    */
} CodexOwnImport;

/* The detected session staged as `name`, duplicated, or NULL. */
static cJSON *codex_detected_session(cJSON *result, const char *name) {
    cJSON *items = cJSON_GetObjectItemCaseSensitive(result, "items");
    cJSON *item;
    usize nl = strlen(name);
    cJSON_ArrayForEach(item, items) {
        const char *type = hist_cjson_str(item, "itemType");
        if (!type || strcmp(type, "SESSIONS") != 0) continue;
        cJSON *details  = cJSON_GetObjectItemCaseSensitive(item, "details");
        cJSON *sessions = details ? cJSON_GetObjectItemCaseSensitive(details, "sessions") : NULL;
        cJSON *s;
        cJSON_ArrayForEach(s, sessions) {
            const char *path = hist_cjson_str(s, "path");
            usize pl = path ? strlen(path) : 0;
            if (pl > nl && path[pl - nl - 1] == '/' && strcmp(path + pl - nl, name) == 0)
                return cJSON_Duplicate(s, 1);
        }
    }
    return NULL;
}

/* The thread Codex made of the import `import_id`, from its completion
 * notification, into `out`. False when the session failed or none came. */
static bool codex_await_import(BridgeProc *p, const char *import_id, char *out, usize cap,
                               bool *answered) {
    *answered = false;
    i64 deadline = bridge_now_ms() + (i64)CODEX_IMPORT_WAIT_SEC * 1000;
    for (;;) {
        i64 left_ms = deadline - bridge_now_ms();
        if (left_ms <= 0) return false;
        char *line = bridge_proc_await_line(p, "import/completed", (i32)((left_ms + 999) / 1000));
        if (!line) return false;
        cJSON *o = cJSON_Parse(line);
        free(line);
        if (!o) continue;
        const char *method = hist_cjson_str(o, "method");
        cJSON *params = cJSON_GetObjectItemCaseSensitive(o, "params");
        const char *id = params ? hist_cjson_str(params, "importId") : NULL;
        if (!method || strcmp(method, "externalAgentConfig/import/completed") != 0 ||
            !id || strcmp(id, import_id) != 0) {
            cJSON_Delete(o);
            continue;
        }
        *answered = true;
        bool ok = false;
        cJSON *res;
        cJSON_ArrayForEach(res, cJSON_GetObjectItemCaseSensitive(params, "itemTypeResults")) {
            const char *type = hist_cjson_str(res, "itemType");
            if (!type || strcmp(type, "SESSIONS") != 0) continue;
            cJSON *s;
            cJSON_ArrayForEach(s, cJSON_GetObjectItemCaseSensitive(res, "successes")) {
                const char *target = hist_cjson_str(s, "target");
                if (target && target[0] && strlen(target) < cap) {
                    hist_copy_bounded(out, cap, target);
                    ok = true;
                    break;
                }
            }
            if (ok) break;
        }
        cJSON_Delete(o);
        return ok;
    }
}

/* One request line `{"jsonrpc":"2.0","id":<id>,"method":<method>,"params":<params>}`;
 * takes `params`. NULL on OOM. */
static char *codex_request(i64 id, const char *method, cJSON *params) {
    cJSON *o = cJSON_CreateObject();
    if (!o) { cJSON_Delete(params); return NULL; }
    cJSON_AddStringToObject(o, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(o, "id", (double)id);
    cJSON_AddStringToObject(o, "method", method);
    cJSON_AddItemToObject(o, "params", params ? params : cJSON_CreateObject());
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!s) return NULL;
    usize n = strlen(s);
    char *line = realloc(s, n + 2);
    if (!line) { free(s); return NULL; }
    line[n] = '\n';
    line[n + 1] = '\0';
    return line;
}

/* ---------------------------------------------- the import's display -- */

static bool codex_same_file(const struct stat *a, const struct stat *b) {
#if defined(__APPLE__)
    bool same_time = a->st_mtimespec.tv_sec == b->st_mtimespec.tv_sec &&
                     a->st_mtimespec.tv_nsec == b->st_mtimespec.tv_nsec;
#elif defined(st_mtime)
    bool same_time = a->st_mtim.tv_sec == b->st_mtim.tv_sec &&
                     a->st_mtim.tv_nsec == b->st_mtim.tv_nsec;
#else
    bool same_time = a->st_mtime == b->st_mtime;
#endif
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino &&
           a->st_size == b->st_size && same_time;
}

/* The regular file at `path`, whole, into a malloc'd buffer. */
static bool codex_read_whole(const char *path, char **out, usize *out_len, struct stat *st) {
    *out = NULL;
    *out_len = 0;
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return false;
    if (fstat(fd, st) != 0 || !S_ISREG(st->st_mode) || st->st_size <= 0 ||
        (i64)st->st_size > CODEX_DISPLAY_MAX) {
        close(fd);
        return false;
    }
    usize len = (usize)st->st_size;
    char *buf = malloc(len + 1);
    usize got = 0;
    while (buf && got < len) {
        ssize_t n = read(fd, buf + got, len - got);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        got += (usize)n;
    }
    close(fd);
    if (!buf || got != len) { free(buf); return false; }
    buf[len] = '\0';
    *out = buf;
    *out_len = len;
    return true;
}

/* Put `data` at `path` in one rename, keeping `mode`. When `expect` is given,
 * only while the file there is still the one `expect` describes. */
static bool codex_replace_file(const char *path, const char *data, usize len, mode_t mode,
                               const struct stat *expect) {
    char tmp[BRIDGE_PATH_CAP + 16];
    int n = snprintf(tmp, sizeof tmp, "%s.liu-XXXXXX", path);
    if (n <= 0 || (usize)n >= sizeof tmp) return false;
    int fd = mkstemp(tmp);
    if (fd < 0) return false;
    bool ok = fchmod(fd, mode & 07777) == 0;
    for (usize off = 0; ok && off < len;) {
        ssize_t w = write(fd, data + off, len - off);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) { ok = false; break; }
        off += (usize)w;
    }
    if (ok && fsync(fd) != 0) ok = false;
    if (close(fd) != 0) ok = false;
    if (ok && expect) {
        struct stat now;
        ok = lstat(path, &now) == 0 && codex_same_file(&now, expect);
    }
    if (ok && rename(tmp, path) != 0) ok = false;
    if (!ok) unlink(tmp);
    return ok;
}

static void codex_count_items(cJSON *turns, CodexDisplayCounts *got) {
    cJSON *turn = NULL;
    cJSON_ArrayForEach(turn, turns) {
        cJSON *items = cJSON_GetObjectItemCaseSensitive(turn, "items");
        cJSON *it = NULL;
        cJSON_ArrayForEach(it, items) {
            const char *t = hist_cjson_str(it, "type");
            if (!t) continue;
            if      (strcmp(t, "userMessage") == 0)  got->user_messages++;
            else if (strcmp(t, "agentMessage") == 0) got->agent_messages++;
            else if (strcmp(t, "fileChange") == 0)   got->file_changes++;
            else if (strcmp(t, "mcpToolCall") == 0)  got->mcp_calls++;
            else if (strcmp(t, "webSearch") == 0)    got->web_searches++;
        }
    }
}

/* Codex's own reader over the rewritten thread, a turn at a time: true when
 * it finds every message and cell the rewrite put there. */
static bool codex_display_check(BridgeProc *p, i64 *id, const char *thread_id,
                                const CodexDisplayCounts *want) {
    CodexDisplayCounts got;
    memset(&got, 0, sizeof got);
    char *cursor = NULL;
    bool ok = false;
    for (u32 page = 0; page < 1000000; page++) {
        cJSON *params = cJSON_CreateObject();
        if (!params) break;
        cJSON_AddStringToObject(params, "threadId", thread_id);
        cJSON_AddNumberToObject(params, "limit", CODEX_CHECK_PAGE);
        if (cursor) cJSON_AddStringToObject(params, "cursor", cursor);
        cJSON_AddStringToObject(params, "sortDirection", "asc");
        cJSON_AddStringToObject(params, "itemsView", "full");
        i64 rid = (*id)++;
        char *req = codex_request(rid, "thread/turns/list", params);
        cJSON *res = req ? codex_call(p, req, rid, CODEX_TIMEOUT_SEC) : NULL;
        free(req);
        cJSON *data = res ? cJSON_GetObjectItemCaseSensitive(res, "data") : NULL;
        if (!cJSON_IsArray(data)) { cJSON_Delete(res); break; }
        codex_count_items(data, &got);
        const char *next = hist_cjson_str(res, "nextCursor");
        free(cursor);
        cursor = (next && next[0]) ? strdup(next) : NULL;
        cJSON_Delete(res);
        if (!cursor) {
            ok = got.user_messages == want->user_messages &&
                 got.agent_messages == want->agent_messages &&
                 got.file_changes == want->file_changes &&
                 got.mcp_calls == want->mcp_calls &&
                 got.web_searches == want->web_searches;
            break;
        }
    }
    free(cursor);
    return ok;
}

/* Draw the imported thread's tool calls as Codex's own cells (see
 * codex_display.h). Codex checks the result — its reader must find what the
 * rewrite put there — and the rollout goes back as the import wrote it when
 * it does not, or when any step fails. Either way the thread opens. */
static void codex_display_apply(BridgeProc *p, i64 *id, const char *thread_id,
                                const char *path, const BridgeHistory *h) {
    char *orig = NULL, *rewritten = NULL;
    usize orig_len = 0, new_len = 0;
    struct stat st;
    CodexDisplayCounts want;
    if (!path || !path[0] || !codex_read_whole(path, &orig, &orig_len, &st)) return;
    if (codex_display_rewrite(orig, orig_len, h, &rewritten, &new_len, &want) &&
        codex_replace_file(path, rewritten, new_len, st.st_mode, &st) &&
        !codex_display_check(p, id, thread_id, &want)) {
        codex_replace_file(path, orig, orig_len, st.st_mode, NULL);
    }
    free(rewritten);
    free(orig);
}

static CodexOwnImport codex_import_own(const BridgeHistory *h, BridgeImportResult *r) {
    char codex_home[BRIDGE_PATH_CAP];
    if (!codex_home_dir(codex_home, sizeof codex_home)) return CODEX_OWN_UNAVAILABLE;

    u32 warnings = 0;
    CodexStage st;
    if (!codex_stage(h, &st, &warnings)) return CODEX_OWN_UNAVAILABLE;

    char env_home[BRIDGE_PATH_CAP + 8], env_codex[BRIDGE_PATH_CAP + 16];
    snprintf(env_home, sizeof env_home, "HOME=%s", st.root);
    snprintf(env_codex, sizeof env_codex, "CODEX_HOME=%s", codex_home);
    const char *env[] = { env_home, env_codex, NULL };
    const char *argv[] = { "codex", "app-server", NULL };

    CodexOwnImport outcome = CODEX_OWN_UNAVAILABLE;
    char *req = NULL;
    BridgeProc *p = bridge_proc_start_env(argv, env);
    if (!p || !codex_handshake(p)) goto done;

    /* Detection, as Codex's own onboarding runs it: the staged session must
     * be among what it finds, or the import refuses it. */
    CodexReply how;
    cJSON *params = cJSON_CreateObject();
    if (!params) goto done;
    cJSON_AddBoolToObject(params, "includeHome", true);
    cJSON_AddNumberToObject(params, "maxSessionAgeDays", 36500);
    req = codex_request(2, "externalAgentConfig/detect", params);
    if (!req) goto done;
    cJSON *det = codex_call_ex(p, req, 2, CODEX_TIMEOUT_SEC, &how);
    free(req);
    req = NULL;
    if (!det) {
        if (how == CODEX_REPLY_NONE) {
            outcome = CODEX_OWN_FAILED;
            snprintf(r->error, sizeof r->error, "Codex app-server did not answer.");
        }
        goto done;
    }
    cJSON *session = codex_detected_session(det, st.name);
    cJSON_Delete(det);
    if (!session) goto done;

    params = cJSON_CreateObject();
    cJSON *items = params ? cJSON_AddArrayToObject(params, "migrationItems") : NULL;
    cJSON *item = items ? cJSON_CreateObject() : NULL;
    cJSON *details = item ? cJSON_CreateObject() : NULL;
    cJSON *sessions = details ? cJSON_AddArrayToObject(details, "sessions") : NULL;
    if (!sessions) {
        cJSON_Delete(session); cJSON_Delete(details); cJSON_Delete(item); cJSON_Delete(params);
        goto done;
    }
    cJSON_AddItemToArray(sessions, session);
    cJSON_AddStringToObject(item, "itemType", "SESSIONS");
    cJSON_AddStringToObject(item, "description", "Continue a session in Codex");
    cJSON_AddNullToObject(item, "cwd");
    cJSON_AddItemToObject(item, "details", details);
    cJSON_AddItemToArray(items, item);
    cJSON_AddStringToObject(params, "source", "liu");
    req = codex_request(3, "externalAgentConfig/import", params);
    if (!req) goto done;
    cJSON *imp = codex_call_ex(p, req, 3, CODEX_TIMEOUT_SEC, &how);
    free(req);
    req = NULL;
    const char *import_id = imp ? hist_cjson_str(imp, "importId") : NULL;
    if (!import_id || !import_id[0]) {
        cJSON_Delete(imp);
        if (how == CODEX_REPLY_NONE) {
            outcome = CODEX_OWN_FAILED;
            snprintf(r->error, sizeof r->error, "Codex app-server did not answer.");
        }
        goto done;
    }
    char import_copy[128];
    hist_copy_bounded(import_copy, sizeof import_copy, import_id);
    cJSON_Delete(imp);

    bool answered = false;
    char thread_id[BRIDGE_SESSION_ID_CAP];
    if (!codex_await_import(p, import_copy, thread_id, sizeof thread_id, &answered)) {
        /* A failure Codex reported created nothing; silence may have. */
        if (!answered) {
            outcome = CODEX_OWN_FAILED;
            snprintf(r->error, sizeof r->error, "Codex did not finish importing the session.");
        }
        goto done;
    }

    /* The thread is Codex's now: whatever follows, the other way would make a
     * second one. */
    outcome = CODEX_OWN_FAILED;
    hist_copy_bounded(r->session_id, sizeof r->session_id, thread_id);
    params = cJSON_CreateObject();
    if (params) cJSON_AddStringToObject(params, "threadId", thread_id);
    req = codex_request(4, "thread/read", params);
    cJSON *read = req ? codex_call_ex(p, req, 4, CODEX_TIMEOUT_SEC, &how) : NULL;
    cJSON *thread = read ? cJSON_GetObjectItemCaseSensitive(read, "thread") : NULL;
    const char *tid  = thread ? hist_cjson_str(thread, "id") : NULL;
    const char *path = thread ? hist_cjson_str(thread, "path") : NULL;
    if (tid && strcmp(tid, thread_id) == 0) {
        i64 next_id = 5;
        codex_display_apply(p, &next_id, thread_id, path, h);
        hist_copy_bounded(r->session_path, sizeof r->session_path, path ? path : "");
        snprintf(r->launch_cmd, sizeof r->launch_cmd, "codex resume %s", thread_id);
        r->warnings += warnings;
        outcome = CODEX_OWN_DONE;
    } else {
        snprintf(r->error, sizeof r->error, "Codex did not confirm the imported thread.");
    }
    cJSON_Delete(read);

done:
    free(req);
    bridge_proc_stop(p);
    codex_stage_remove(&st);
    return outcome;
}

static bool codex_import(const BridgeHistory *h, BridgeImportResult *r) {
    r->mode = BRIDGE_IMPORT_NATIVE_HISTORY;
    if (!h->cwd || !h->cwd[0]) {
        /* A thread without a cwd would be placed wherever Liu happens to be,
         * and Codex scopes resume by directory — the user would not find it
         * again. */
        snprintf(r->error, sizeof r->error,
                 "Source session does not record a working directory.");
        return false;
    }
    switch (codex_import_own(h, r)) {
        case CODEX_OWN_DONE:        return true;
        case CODEX_OWN_FAILED:      return false;
        case CODEX_OWN_UNAVAILABLE: break;
    }
    r->error[0] = '\0';
    r->session_id[0] = r->session_path[0] = r->launch_cmd[0] = '\0';
    return codex_import_inject(h, r);
}

/* The rollout file thread/start named is Codex's own store for the thread, so
 * its existence and non-emptiness is a check that does not cost another
 * app-server start. The authoritative check — "does Codex itself resolve this
 * thread id" — already ran inside the import, on the connection that created
 * it; repeating it here would pay ~4 s of MCP server startup to ask the same
 * question a second time. */
static bool codex_validate(const BridgeImportResult *r) {
    if (!r->session_id[0]) return false;
    if (!r->session_path[0]) return false;
    struct stat st;
    return lstat(r->session_path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

const BridgeTarget g_bridge_target_codex = {
    .tool                  = CHAT_TOOL_CODEX,
    .mode                  = BRIDGE_IMPORT_NATIVE_HISTORY,
    .available             = codex_available,
    .supports_full_history = codex_full_history,
    .import_history        = codex_import,
    .validate_import       = codex_validate,
    .handoff_launch        = "codex %s",
};
