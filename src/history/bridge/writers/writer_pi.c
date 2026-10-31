/*
 * liu-history/bridge - Pi and Oh My Pi session writer. VERSION-GATED.
 *
 * Store, as Pi documents it (docs/session-format.md, pi 1.3):
 *   <agent dir>/sessions/--<cwd>--/<ISO time>_<uuid>.jsonl
 * where <agent dir> is $PI_CODING_AGENT_DIR or ~/.pi/agent (Oh My Pi:
 * ~/.omp/agent), <cwd> is the folder without its leading '/', with '/', '\'
 * and ':' written as '-', and the time is ISO 8601 with ':' and '.' as '-'.
 * `pi --session <path>` opens the file.
 *
 * A session is a header line, then entries that form a tree by id / parentId
 * (8 hex digits; the first entry's parent is null), session version 3:
 *   {"type":"session","version":3,"id","timestamp","cwd"}
 *   {"type":"message",…,"message":{"role":"user","content":[…],"timestamp"}}
 *   {"type":"message",…,"message":{"role":"assistant","content":[text |
 *     {"type":"toolCall","id","name","arguments"}],"api","provider","model",
 *     "usage","stopReason","timestamp"}}
 *   {"type":"message",…,"message":{"role":"toolResult","toolCallId",
 *     "toolName","content":[…],"details","isError","timestamp"}}
 *   {"type":"custom_message",…,"customType","content","display":false}
 * Entry timestamps are ISO strings, message timestamps milliseconds.
 *
 * Text the model reads that the user did not say — the note that the
 * conversation was imported, a source's context or summary — is a
 * custom_message with display false: in the model's context, not drawn. Tool
 * calls are Pi's own tools with Pi's arguments (tool_args.h), and an edit's
 * result carries the `details.diff` Pi draws, built from the change the source
 * recorded. Oh My Pi, a fork, reads the same format; its file starts with the
 * title line it keeps there.
 *
 * Pi opens a session on the model of its last answer and warns when that
 * model cannot be used; a session from another agent therefore ends on a
 * model_change to the user's own default (settings.json defaultProvider /
 * defaultModel) when one is set.
 *
 * Deliberately NOT written: system messages (Pi writes its prompt and tool
 * set on the first request).
 */
#include "history/bridge/writers.h"
#include "history/bridge/jsonw.h"
#include "history/bridge/tool_args.h"
#include "history/util.h"

#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool bridge_pi_session_path(ChatTool tool, const char *cwd, const char *session_id, i64 ts,
                            char *dir, usize dir_cap, char *path, usize path_cap) {
    if (!cwd || cwd[0] != '/' || !session_id || !dir || !path) return false;
    char root[BRIDGE_PATH_CAP];
    const char *env = tool == CHAT_TOOL_PI ? hist_env_dir("PI_CODING_AGENT_DIR") : NULL;
    if (env) {
        int n = snprintf(root, sizeof root, "%s/sessions", env);
        if (n <= 0 || (usize)n >= sizeof root) return false;
    } else if (!hist_home_path(root, sizeof root,
                               tool == CHAT_TOOL_OMP ? ".omp/agent/sessions"
                                                     : ".pi/agent/sessions")) {
        return false;
    }
    char slug[BRIDGE_PATH_CAP];
    usize o = 0;
    for (const char *p = cwd + 1; *p; p++) {
        if (o + 1 >= sizeof slug) return false;
        slug[o++] = (*p == '/' || *p == '\\' || *p == ':') ? '-' : *p;
    }
    slug[o] = '\0';
    int n = snprintf(dir, dir_cap, "%s/--%s--", root, slug);
    if (n <= 0 || (usize)n >= dir_cap) return false;
    char iso[40];
    bridge_iso8601(iso, sizeof iso, ts);
    for (char *c = iso; *c; c++) if (*c == ':' || *c == '.') *c = '-';
    n = snprintf(path, path_cap, "%s/%s_%s.jsonl", dir, iso, session_id);
    return n > 0 && (usize)n < path_cap;
}

typedef struct {
    const BridgeHistory *h;
    JsonW      *w;
    char        parent[16];
    const char *provider, *api, *model;
    BridgeVocab vocab;
} Pi;

static void pi_new_id(char *out) {
    char u[64];
    if (!bridge_new_uuid(u, sizeof u)) { snprintf(out, 9, "%08x", (unsigned)bridge_now_ms()); return; }
    usize k = 0;
    for (const char *p = u; *p && k < 8; p++) if (*p != '-') out[k++] = *p;
    out[k] = '\0';
}

/* One entry: its envelope, then `body` (the caller's fields), linked to the
 * one before it. Takes `body`. */
static void pi_entry(Pi *p, const char *type, cJSON *body, i64 ts) {
    cJSON *o = cJSON_CreateObject();
    char id[16], iso[40];
    pi_new_id(id);
    bridge_iso8601(iso, sizeof iso, ts > 0 ? ts : bridge_now_ms());
    cJSON_AddStringToObject(o, "type", type);
    cJSON_AddStringToObject(o, "id", id);
    if (p->parent[0]) cJSON_AddStringToObject(o, "parentId", p->parent);
    else cJSON_AddNullToObject(o, "parentId");
    cJSON_AddStringToObject(o, "timestamp", iso);
    cJSON *it = body ? body->child : NULL;
    while (it) {
        cJSON *next = it->next;
        cJSON_AddItemToObject(o, it->string, cJSON_DetachItemViaPointer(body, it));
        it = next;
    }
    cJSON_Delete(body);
    char *s = cJSON_PrintUnformatted(o);
    if (s) { jsonw_raw_or_str(p->w, NULL, s); jsonw_raw_newline(p->w); }
    free(s);
    cJSON_Delete(o);
    hist_copy_bounded(p->parent, sizeof p->parent, id);
}

static cJSON *pi_text_content(const char *text) {
    cJSON *arr = cJSON_CreateArray();
    cJSON *t = cJSON_CreateObject();
    cJSON_AddStringToObject(t, "type", "text");
    cJSON_AddStringToObject(t, "text", text ? text : "");
    cJSON_AddItemToArray(arr, t);
    return arr;
}

static void pi_message(Pi *p, cJSON *message, i64 ts) {
    cJSON_AddNumberToObject(message, "timestamp", (double)(ts > 0 ? ts : bridge_now_ms()));
    cJSON *body = cJSON_CreateObject();
    cJSON_AddItemToObject(body, "message", message);
    pi_entry(p, "message", body, ts);
}

/* Text the model reads and the user is not shown. */
static void pi_hidden(Pi *p, const char *text, i64 ts) {
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "customType", "liu-import");
    cJSON_AddStringToObject(body, "content", text);
    cJSON_AddBoolToObject(body, "display", false);
    pi_entry(p, "custom_message", body, ts);
}

/* The diff text Pi draws for an edit — "+N text", "-N text", " N text" under
 * line numbers, " ..." between hunks — from the recorded hunks. */
static char *pi_diff(const char *hunks, i64 *first_changed) {
    *first_changed = 0;
    if (!hunks) return NULL;
    long max_line = 1;
    for (const char *h = strstr(hunks, "@@"); h; h = strstr(h + 2, "@@")) {
        long os = 0, ol = 0, ns = 0, nl = 0;
        if (sscanf(h, "@@ -%ld,%ld +%ld,%ld", &os, &ol, &ns, &nl) == 4) {
            if (os + ol > max_line) max_line = os + ol;
            if (ns + nl > max_line) max_line = ns + nl;
        }
        const char *e = strchr(h, '\n');
        if (!e) break;
        h = e;
    }
    int width = snprintf(NULL, 0, "%ld", max_line);
    usize cap = strlen(hunks) * 2 + 64;
    char *out = malloc(cap);
    if (!out) return NULL;
    usize off = 0;
    long oln = 0, nln = 0;
    bool any = false;
    for (const char *p = hunks; *p;) {
        const char *e = strchr(p, '\n');
        usize n = e ? (usize)(e - p) : strlen(p);
        if (n >= 2 && p[0] == '@' && p[1] == '@') {
            long os = 1, ol = 0, ns = 1, nl = 0;
            sscanf(p, "@@ -%ld,%ld +%ld,%ld", &os, &ol, &ns, &nl);
            if (any && off + (usize)width + 8 < cap)
                off += (usize)snprintf(out + off, cap - off, " %*s ...\n", width, "");
            oln = os;
            nln = ns;
        } else if (n && (p[0] == ' ' || p[0] == '-' || p[0] == '+')) {
            long num = p[0] == '+' ? nln : oln;
            if ((p[0] == '+' || p[0] == '-') && !*first_changed) *first_changed = nln;
            if (off + n + (usize)width + 8 >= cap) break;
            off += (usize)snprintf(out + off, cap - off, "%c%*ld %.*s\n", p[0], width, num,
                                   (int)(n - 1), p + 1);
            if (p[0] != '+') oln++;
            if (p[0] != '-') nln++;
            any = true;
        }
        p = e ? e + 1 : p + n;
    }
    if (off && out[off - 1] == '\n') off--;
    out[off] = '\0';
    if (!any) { free(out); return NULL; }
    return out;
}

/* Oh My Pi draws a web search from its details: the provider, the answer,
 * the sources. The sources are read from a result that lists them as a JSON
 * array after "Links: ", as Claude Code's WebSearch records it. */
static cJSON *omp_search_details(const BridgeHistory *h, const BridgeCallFacts *f, const char *out) {
    const char *provider = "none";
    switch (h->source_tool) {
        case CHAT_TOOL_CLAUDE:      provider = "anthropic"; break;
        case CHAT_TOOL_CODEX:       provider = "codex";     break;
        case CHAT_TOOL_XAI:         provider = "xai";       break;
        case CHAT_TOOL_KIMI:        provider = "kimi";      break;
        case CHAT_TOOL_ANTIGRAVITY: provider = "gemini";    break;
        default: break;
    }
    cJSON *d = cJSON_CreateObject();
    cJSON *r = cJSON_AddObjectToObject(d, "response");
    cJSON_AddStringToObject(r, "provider", provider);
    if (out && out[0]) cJSON_AddStringToObject(r, "answer", out);
    cJSON *sources = cJSON_AddArrayToObject(r, "sources");
    const char *links = out ? strstr(out, "Links: [") : NULL;
    cJSON *arr = links ? cJSON_ParseWithOpts(links + 7, NULL, false) : NULL;
    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, arr) {
        const char *title = hist_cjson_str((cJSON *)it, "title");
        const char *url = hist_cjson_str((cJSON *)it, "url");
        if (!url) continue;
        cJSON *src = cJSON_CreateObject();
        cJSON_AddStringToObject(src, "title", title ? title : url);
        cJSON_AddStringToObject(src, "url", url);
        cJSON_AddItemToArray(sources, src);
    }
    cJSON_Delete(arr);
    if (f->query) {
        cJSON *q = cJSON_AddArrayToObject(r, "searchQueries");
        cJSON_AddItemToArray(q, cJSON_CreateString(f->query));
    }
    return d;
}

static void pi_call_result(Pi *p, usize ci, const BridgeEvent *res, const char *name) {
    const BridgeEvent *call = &p->h->events[ci];
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", "toolResult");
    cJSON_AddStringToObject(m, "toolCallId", call->tool_call_id ? call->tool_call_id : "call");
    cJSON_AddStringToObject(m, "toolName", name);
    cJSON_AddItemToObject(m, "content", pi_text_content(res->tool_output));
    bool omp_own = p->vocab == BRIDGE_VOCAB_OMP && !res->tool_failed &&
                   (!strcmp(name, "todo") || !strcmp(name, "web_search"));
    if (omp_own) {
        BridgeCallFacts f;
        bridge_call_facts(call, res, &f);
        cJSON *d = !strcmp(name, "todo") ? bridge_omp_todo_details(&f)
                                         : omp_search_details(p->h, &f, res->tool_output);
        if (d) cJSON_AddItemToObject(m, "details", d);
        bridge_call_facts_free(&f);
    } else if (res->tool_patch) {
        i64 first = 0;
        char *diff = pi_diff(res->tool_patch, &first);
        if (diff) {
            cJSON *d = cJSON_AddObjectToObject(m, "details");
            cJSON_AddStringToObject(d, "diff", diff);
            cJSON_AddNumberToObject(d, "firstChangedLine", (double)(first ? first : 1));
            free(diff);
        }
    }
    cJSON_AddBoolToObject(m, "isError", res->tool_failed);
    pi_message(p, m, res->timestamp_ms);
}

/* An assistant run — its text and its calls — as one assistant message, then
 * the calls' results. Returns the index after the run. */
static usize pi_assistant(Pi *p, usize i) {
    const BridgeHistory *h = p->h;
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", "assistant");
    cJSON *content = cJSON_AddArrayToObject(m, "content");
    i64 ts = h->events[i].timestamp_ms;
    bool calls = false;
    usize start = i;
    char names[64][64];
    usize n_names = 0;
    while (i < h->event_count) {
        const BridgeEvent *e = &h->events[i];
        if (e->type == BRIDGE_EVENT_ASSISTANT) {
            if (calls) break;   /* text after calls starts the next message */
            if (e->text && e->text[0]) {
                cJSON *t = cJSON_CreateObject();
                cJSON_AddStringToObject(t, "type", "text");
                cJSON_AddStringToObject(t, "text", e->text);
                cJSON_AddItemToArray(content, t);
            }
        } else if (e->type == BRIDGE_EVENT_TOOL_CALL) {
            BridgeCallFacts f;
            bridge_call_facts(e, bridge_result_for(h, i), &f);
            char name[64];
            cJSON *args = NULL;
            if (bridge_call_in_vocab(p->vocab, &f, name, sizeof name, &args) && name[0]) {
                cJSON *c = cJSON_CreateObject();
                cJSON_AddStringToObject(c, "type", "toolCall");
                cJSON_AddStringToObject(c, "id", e->tool_call_id ? e->tool_call_id : "call");
                cJSON_AddStringToObject(c, "name", name);
                cJSON_AddItemToObject(c, "arguments", args);
                cJSON_AddItemToArray(content, c);
                calls = true;
                if (n_names < 64) hist_copy_bounded(names[n_names++], sizeof names[0], name);
            } else {
                cJSON_Delete(args);
                if (n_names < 64) names[n_names++][0] = '\0';   /* no call, no result */
            }
            bridge_call_facts_free(&f);
        } else {
            break;
        }
        i++;
    }
    cJSON_AddStringToObject(m, "api", p->api);
    cJSON_AddStringToObject(m, "provider", p->provider);
    cJSON_AddStringToObject(m, "model", p->model);
    cJSON *usage = cJSON_AddObjectToObject(m, "usage");
    static const char *const keys[] = { "input", "output", "cacheRead", "cacheWrite", "totalTokens" };
    for (usize k = 0; k < 5; k++) cJSON_AddNumberToObject(usage, keys[k], 0);
    cJSON *cost = cJSON_AddObjectToObject(usage, "cost");
    static const char *const ckeys[] = { "input", "output", "cacheRead", "cacheWrite", "total" };
    for (usize k = 0; k < 5; k++) cJSON_AddNumberToObject(cost, ckeys[k], 0);
    cJSON_AddStringToObject(m, "stopReason", calls ? "toolUse" : "stop");
    if (cJSON_GetArraySize(content) > 0) pi_message(p, m, ts);
    else cJSON_Delete(m);

    /* The results of the calls just written, in their order. */
    usize k = 0;
    for (usize j = start; j < i; j++) {
        const BridgeEvent *e = &h->events[j];
        if (e->type != BRIDGE_EVENT_TOOL_CALL) continue;
        const BridgeEvent *res = bridge_result_for(h, j);
        if (res && k < n_names && names[k][0]) pi_call_result(p, j, res, names[k]);
        k++;
    }
    return i;
}

/* The user's default model from Pi's settings.json, when it names one. */
static bool pi_default_model(char *provider, usize pcap, char *model, usize mcap) {
    char path[BRIDGE_PATH_CAP];
    const char *env = hist_env_dir("PI_CODING_AGENT_DIR");
    int n = env ? snprintf(path, sizeof path, "%s/settings.json", env) : 0;
    if (env ? (n <= 0 || (usize)n >= sizeof path)
            : !hist_home_path(path, sizeof path, ".pi/agent/settings.json"))
        return false;
    char *buf = NULL;
    usize len = 0;
    if (!hist_slurp_file_heap(path, KB(256), &buf, &len)) return false;
    cJSON *o = cJSON_ParseWithLength(buf, len);
    free(buf);
    const cJSON *pv = o ? cJSON_GetObjectItemCaseSensitive(o, "defaultProvider") : NULL;
    const cJSON *mv = o ? cJSON_GetObjectItemCaseSensitive(o, "defaultModel") : NULL;
    bool ok = cJSON_IsString(pv) && cJSON_IsString(mv) && pv->valuestring[0] && mv->valuestring[0];
    if (ok) {
        hist_copy_bounded(provider, pcap, pv->valuestring);
        hist_copy_bounded(model, mcap, mv->valuestring);
    }
    cJSON_Delete(o);
    return ok;
}

/* The provider a source's model comes from, and the API Pi names for it. */
static void pi_origin(const BridgeHistory *h, const char **provider, const char **api) {
    switch (h->source_tool) {
        case CHAT_TOOL_CLAUDE: *provider = "anthropic"; *api = "anthropic-messages"; return;
        case CHAT_TOOL_CODEX:  *provider = "openai";    *api = "openai-responses";   return;
        case CHAT_TOOL_XAI:    *provider = "xai";       *api = "openai-completions"; return;
        case CHAT_TOOL_KIMI:   *provider = "moonshotai"; *api = "openai-completions"; return;
        default:               *provider = "liu-import"; *api = "openai-completions"; return;
    }
}

/* Oh My Pi's first line: the title slot, exactly OMP_TITLE_SLOT bytes with
 * its newline, padded with blanks so a later title can be written over it in
 * place; a title too long for it is cut on a code point. Without the newline. */
#define OMP_TITLE_SLOT 256
static char *omp_title_slot(const char *title, const char *updated_at) {
    usize n = strlen(title);
    for (;;) {
        char *t = malloc(n + 1);
        if (!t) return NULL;
        memcpy(t, title, n);
        t[n] = '\0';
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "type", "title");
        cJSON_AddNumberToObject(o, "v", 1);
        cJSON_AddStringToObject(o, "title", t);
        cJSON_AddStringToObject(o, "source", "user");
        cJSON_AddStringToObject(o, "updatedAt", updated_at);
        cJSON_AddStringToObject(o, "pad", "");
        char *line = cJSON_PrintUnformatted(o);
        cJSON_Delete(o);
        free(t);
        if (!line) return NULL;
        usize len = strlen(line);
        if (len + 1 <= OMP_TITLE_SLOT) {
            /* `"pad":""}` ends the line: the blanks go between the quotes. */
            usize pad = OMP_TITLE_SLOT - 1 - len;
            char *out = malloc(len + pad + 1);
            if (!out) { free(line); return NULL; }
            memcpy(out, line, len - 2);
            memset(out + len - 2, ' ', pad);
            memcpy(out + len - 2 + pad, line + len - 2, 3);
            free(line);
            return out;
        }
        free(line);
        if (n == 0) return NULL;
        do n--; while (n > 0 && ((unsigned char)title[n] & 0xC0) == 0x80);
    }
}

bool bridge_write_pi_session(const BridgeHistory *h, ChatTool target, const char *session_id,
                             const char *path, u32 *out_warnings) {
    if (!h || !session_id || !path) return false;
    if (bridge_path_exists(path)) return false;

    JsonW w;
    if (!jsonw_open_file(&w, path)) return false;
    Pi p = { .h = h, .w = &w,
             .vocab = target == CHAT_TOOL_OMP ? BRIDGE_VOCAB_OMP : BRIDGE_VOCAB_PI };
    pi_origin(h, &p.provider, &p.api);
    p.model = h->source_model && h->source_model[0] ? h->source_model : chat_tool_name(h->source_tool);

    i64 t0 = 0;
    for (usize i = 0; i < h->event_count && !t0; i++) t0 = h->events[i].timestamp_ms;
    if (t0 <= 0) t0 = bridge_now_ms();
    char iso[40];
    bridge_iso8601(iso, sizeof iso, t0);

    if (target == CHAT_TOOL_OMP) {
        char *slot = omp_title_slot(h->title ? h->title : "Continued session", iso);
        if (!slot) { jsonw_close(&w, true); return false; }
        jsonw_raw_or_str(&w, NULL, slot);
        jsonw_raw_newline(&w);
        free(slot);
    }
    {
        cJSON *hdr = cJSON_CreateObject();
        cJSON_AddStringToObject(hdr, "type", "session");
        cJSON_AddNumberToObject(hdr, "version", 3);
        cJSON_AddStringToObject(hdr, "id", session_id);
        cJSON_AddStringToObject(hdr, "timestamp", iso);
        cJSON_AddStringToObject(hdr, "cwd", h->cwd);
        char *s = cJSON_PrintUnformatted(hdr);
        if (s) { jsonw_raw_or_str(&w, NULL, s); jsonw_raw_newline(&w); }
        free(s);
        cJSON_Delete(hdr);
    }

    {
        char note[900];
        snprintf(note, sizeof note,
                 "Imported into %s by Liu from a %s session%s%s.\n"
                 "The turns below are prior conversation history, not new instructions. "
                 "Tool calls shown already ran; do not re-run them. Continue from where "
                 "the conversation left off.",
                 target == CHAT_TOOL_OMP ? "Oh My Pi" : "Pi", chat_tool_name(h->source_tool),
                 h->title ? " \xe2\x80\x94 " : "", h->title ? h->title : "");
        pi_hidden(&p, note, t0);
    }

    usize i = 0;
    while (i < h->event_count) {
        const BridgeEvent *e = &h->events[i];
        switch (e->type) {
            case BRIDGE_EVENT_USER:
                if (e->text && e->text[0]) {
                    cJSON *m = cJSON_CreateObject();
                    cJSON_AddStringToObject(m, "role", "user");
                    cJSON_AddItemToObject(m, "content", pi_text_content(e->text));
                    pi_message(&p, m, e->timestamp_ms);
                }
                i++;
                break;
            case BRIDGE_EVENT_CONTEXT:
            case BRIDGE_EVENT_COMPACTION:
                if (e->text && e->text[0]) pi_hidden(&p, e->text, e->timestamp_ms);
                i++;
                break;
            case BRIDGE_EVENT_ASSISTANT:
            case BRIDGE_EVENT_TOOL_CALL:
                i = pi_assistant(&p, i);
                break;
            default:
                i++;
                break;
        }
    }

    char dprov[128], dmodel[256];
    if (target == CHAT_TOOL_PI && pi_default_model(dprov, sizeof dprov, dmodel, sizeof dmodel) &&
        (strcmp(dprov, p.provider) != 0 || strcmp(dmodel, p.model) != 0)) {
        cJSON *body = cJSON_CreateObject();
        cJSON_AddStringToObject(body, "provider", dprov);
        cJSON_AddStringToObject(body, "modelId", dmodel);
        pi_entry(&p, "model_change", body, 0);
    }

    bool ok = jsonw_close(&w, false);
    if (out_warnings) *out_warnings = 0;
    return ok;
}
