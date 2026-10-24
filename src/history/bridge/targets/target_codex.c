/*
 * This adapter uses Codex app-server over stdio JSON-RPC. thread/start creates
 * a thread from cwd; thread/inject_items appends model-visible Responses API
 * items. The returned thread ID is used with codex resume <threadId>.
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
#include "history/bridge/compatibility.h"
#include "history/bridge/jsonw.h"
#include "history/bridge/tool_map.h"
#include "history/util.h"

#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* app-server startup pays for MCP server launches and a config read, which
 * takes seconds with several MCP servers configured. 90 s is "something is
 * wrong", not "this is slow". */
#define CODEX_TIMEOUT_SEC 90
#define CODEX_REPLY_CAP   ((usize)MB(2))

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
static cJSON *codex_call(BridgeProc *p, const char *req, i64 id, i32 timeout_sec) {
    if (!bridge_proc_send(p, req, strlen(req))) return NULL;
    char needle[32];
    snprintf(needle, sizeof needle, "\"id\":%lld", (long long)id);
    char *line = bridge_proc_await_line(p, needle, timeout_sec);
    if (!line) return NULL;

    cJSON *o = cJSON_Parse(line);
    free(line);
    if (!o) return NULL;
    /* The substring test can match an id inside a notification's payload, so
     * the parsed object is checked properly before it is believed. */
    i64 got = 0;
    if (!hist_cjson_i64(o, "id", &got) || got != id) { cJSON_Delete(o); return NULL; }
    cJSON *res = cJSON_GetObjectItemCaseSensitive(o, "result");
    if (!res) { cJSON_Delete(o); return NULL; }      /* an error response */
    cJSON *dup = cJSON_Duplicate(res, 1);
    cJSON_Delete(o);
    return dup;
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

static bool codex_import(const BridgeHistory *h, BridgeImportResult *r) {
    r->mode = BRIDGE_IMPORT_NATIVE_HISTORY;

    const char *cwd = (h->cwd && h->cwd[0]) ? h->cwd : NULL;
    if (!cwd) {
        /* thread/start without a cwd would place the thread wherever Liu
         * happens to be, and Codex scopes resume by directory — the user
         * would not find it again. */
        snprintf(r->error, sizeof r->error,
                 "Source session does not record a working directory.");
        return false;
    }

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
};
