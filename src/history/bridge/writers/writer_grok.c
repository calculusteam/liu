/*
 * liu-history/bridge - Grok session writer. VERSION-GATED.
 *
 * Grok stores a session as a DIRECTORY, not a file:
 *   ~/.grok/sessions/<percent-encoded-cwd>/<uuid>/
 * ($GROK_HOME overrides ~/.grok.) A session directory holds these files:
 *
 *   chat_history.jsonl   the conversation the model reads   <- written
 *   updates.jsonl        the conversation Grok draws        <- written
 *   summary.json         id, cwd, times, title              <- written
 *   system_prompt.txt    the harness prompt                 <- NOT written
 *   events.jsonl, rewind_points.jsonl, signals.json,
 *   prompt_context.json, announcement_state.json, *.lock   <- NOT written
 *
 * Grok draws a resumed session from updates.jsonl alone: the ACP session
 * updates it recorded while the turns ran (user_message_chunk,
 * agent_message_chunk, tool_call and its tool_call_updates, turn_completed).
 * A session with a history and no updates opens on an empty screen.
 *
 * The omissions are deliberate. The lock
 * files are a running process's business and creating them for a session
 * nobody has opened is a way to wedge one. `system_prompt.txt` is Grok's own
 * prompt, which it regenerates per version and per model; a stale copy
 * imported from Liu would pin an old one. The rest are per-run telemetry and
 * UI state that a session legitimately starts without — Grok recreates them
 * on first open, which is exactly what a resumed-but-never-run session should
 * look like.
 *
 * The percent encoding of the project directory is the one non-obvious bit:
 * ~/.grok/sessions/%2Fprivate%2Ftmp%2Fgrokprobe/<uuid>/ — the absolute cwd
 * with '/' as %2F, uppercase hex. Sessions may also sit directly under the
 * root with no project directory (a "global" session), but a session written
 * without one loses its cwd association and would not be offered when the
 * user is inside the project, so this always writes the project form.
 *
 * chat_history.jsonl record shapes, as of grok 1.0.70:
 *   {"type":"user","content":[{"type":"text","text":"…"}],"prompt_index":N}
 *   {"type":"user","content":[…],"synthetic_reason":"system_reminder"}
 *   {"type":"assistant","content":"…","tool_calls":[{"id","name",
 *    "arguments":"<JSON string>"}],"model_id":"…"}
 *   {"type":"tool_result","tool_call_id":"…","content":"…"}
 *
 * A call's `arguments` is a JSON STRING: an object there makes Grok set the
 * whole file aside as corrupt. A user turn carries an increasing
 * `prompt_index`, which rewind and prompt history index on. Text the model is
 * to read that the user did not type is a system reminder, sent and not
 * drawn. Tool calls are Grok's own tools with its arguments (tool_args.h).
 */
#include "history/bridge/writers.h"
#include "history/bridge/jsonw.h"
#include "history/bridge/tool_args.h"
#include "history/bridge/tool_map.h"
#include "history/util.h"

#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Percent-encode a path the way Grok names its project buckets: every byte
 * outside [A-Za-z0-9._-~] becomes %XX with UPPERCASE hex. */
static bool percent_encode(const char *s, char *out, usize cap) {
    static const char hex[] = "0123456789ABCDEF";
    usize o = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        bool safe = (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                    (*p >= '0' && *p <= '9') ||
                    *p == '.' || *p == '_' || *p == '-' || *p == '~';
        if (safe) {
            if (o + 1 >= cap) return false;
            out[o++] = (char)*p;
        } else {
            if (o + 3 >= cap) return false;
            out[o++] = '%';
            out[o++] = hex[*p >> 4];
            out[o++] = hex[*p & 0x0F];
        }
    }
    if (o >= cap) return false;
    out[o] = '\0';
    return true;
}

bool bridge_grok_session_dir(const char *cwd, const char *session_id,
                             char *out, usize cap) {
    if (!cwd || cwd[0] != '/' || !session_id || !out) return false;
    char root[BRIDGE_PATH_CAP];
    if (!hist_resolve_root(root, sizeof root, "GROK_HOME", "sessions",
                           ".grok/sessions"))
        return false;
    char enc[BRIDGE_PATH_CAP];
    if (!percent_encode(cwd, enc, sizeof enc)) return false;
    int w = snprintf(out, cap, "%s/%s/%s", root, enc, session_id);
    return w > 0 && (usize)w < cap;
}

/* The session's first and last times. */
static void grok_span(const BridgeHistory *h, i64 *t0, i64 *t1) {
    *t0 = *t1 = 0;
    for (usize i = 0; i < h->event_count; i++) {
        i64 t = h->events[i].timestamp_ms;
        if (t <= 0) continue;
        if (!*t0 || t < *t0) *t0 = t;
        if (t > *t1) *t1 = t;
    }
    if (!*t0) *t0 = bridge_now_ms();
    if (!*t1) *t1 = *t0;
}

/* The model Grok opens a session on: the user's own default from
 * $GROK_HOME/config.toml ([models] default = "…"), into `out`. False when the
 * file names none. */
static bool grok_default_model(char *out, usize cap) {
    char path[BRIDGE_PATH_CAP];
    if (!hist_resolve_root(path, sizeof path, "GROK_HOME", "config.toml", ".grok/config.toml"))
        return false;
    char *buf = NULL;
    usize len = 0;
    if (!hist_slurp_file_heap(path, KB(256), &buf, &len)) return false;
    bool in_models = false, found = false;
    for (char *line = buf; line && *line && !found;) {
        char *e = strchr(line, '\n');
        if (e) *e = '\0';
        while (*line == ' ' || *line == '\t') line++;
        if (*line == '[') in_models = strncmp(line, "[models]", 8) == 0;
        else if (in_models && strncmp(line, "default", 7) == 0) {
            const char *q = strchr(line, '"');
            const char *r = q ? strchr(q + 1, '"') : NULL;
            if (r && r > q + 1 && (usize)(r - q - 1) < cap) {
                memcpy(out, q + 1, (usize)(r - q - 1));
                out[r - q - 1] = '\0';
                found = true;
            }
        }
        line = e ? e + 1 : NULL;
    }
    free(buf);
    return found;
}

static bool write_summary(const BridgeHistory *h, const char *session_id,
                          const char *dir, u32 records) {
    char path[BRIDGE_PATH_CAP];
    int n = snprintf(path, sizeof path, "%s/summary.json", dir);
    if (n <= 0 || (usize)n >= sizeof path) return false;
    if (bridge_path_exists(path)) return false;

    i64 t0, t1;
    grok_span(h, &t0, &t1);
    char iso0[40], iso1[40];
    bridge_iso8601(iso0, sizeof iso0, t0);
    bridge_iso8601(iso1, sizeof iso1, t1);

    JsonW w;
    if (!jsonw_open_file(&w, path)) return false;
    jsonw_obj_begin(&w, NULL);
    jsonw_obj_begin(&w, "info");
    jsonw_str(&w, "id", session_id);
    jsonw_str(&w, "cwd", h->cwd);
    jsonw_obj_end(&w);
    jsonw_str(&w, "agent_id", session_id);
    jsonw_str(&w, "session_summary", h->title ? h->title : "");
    jsonw_str(&w, "generated_title", h->title ? h->title : "");
    jsonw_str(&w, "created_at", iso0);
    jsonw_str(&w, "updated_at", iso1);
    jsonw_i64(&w, "num_messages", records);
    jsonw_i64(&w, "num_chat_messages", records);
    /* Required. Grok opens a session on it and says so when it cannot, so
     * another agent's model is not it: the user's own default is. */
    {
        char model[128];
        if (h->source_tool == CHAT_TOOL_XAI && h->source_model && h->source_model[0])
            hist_copy_bounded(model, sizeof model, h->source_model);
        else if (!grok_default_model(model, sizeof model))
            hist_copy_bounded(model, sizeof model, "grok-4.6");
        jsonw_str(&w, "current_model_id", model);
    }
    jsonw_i64(&w, "next_trace_turn", 1);
    jsonw_i64(&w, "chat_format_version", 1);
    jsonw_str(&w, "last_active_at", iso1);
    /* session_kind matters: the scanner (and Grok's own picker) hides
     * "subagent" rows, so an imported session must NOT claim to be one. */
    jsonw_str(&w, "session_kind", "user");
    /* Deliberately absent: sandbox_profile, reasoning_effort, request_id,
     * grok_home. The first two are policy the target must choose for itself;
     * the last two are per-run identity. */
    jsonw_obj_end(&w);
    return jsonw_close(&w, false);
}

/* ------------------------------------------------------------ records --- */

typedef struct {
    const BridgeHistory *h;
    const char *sid;
    JsonW      *chat;     /* chat_history.jsonl: what the model reads       */
    JsonW      *ui;       /* updates.jsonl: what Grok draws                 */
    u32         seq;      /* updates written, for their event ids           */
    u32         records;  /* chat records written                           */
    u32         prompt;   /* prompt_index of the next user turn             */
    bool        turn;     /* a turn is open and owes its turn_completed     */
    i64         turn_t0, turn_t1;
    const char *model;
} Grok;

static void grok_line(JsonW *w, cJSON *o) {
    char *s = o ? cJSON_PrintUnformatted(o) : NULL;
    if (s) {
        jsonw_raw_or_str(w, NULL, s);
        jsonw_raw_newline(w);
    }
    free(s);
    cJSON_Delete(o);
}

static void grok_chat(Grok *g, cJSON *o) {
    grok_line(g->chat, o);
    g->records++;
}

/* One display update, in the envelope Grok writes them in. */
static void grok_update(Grok *g, cJSON *update, const char *method, i64 ts) {
    if (ts <= 0) ts = bridge_now_ms();
    cJSON *o = cJSON_CreateObject();
    if (o) {
        cJSON_AddNumberToObject(o, "timestamp", (double)(ts / 1000));
        cJSON_AddStringToObject(o, "method", method ? method : "session/update");
    }
    cJSON *params = o ? cJSON_AddObjectToObject(o, "params") : NULL;
    if (!params) { cJSON_Delete(o); cJSON_Delete(update); return; }
    cJSON_AddStringToObject(params, "sessionId", g->sid);
    cJSON_AddItemToObject(params, "update", update);
    cJSON *meta = cJSON_AddObjectToObject(params, "_meta");
    char id[160];
    snprintf(id, sizeof id, "%s-%u", g->sid, ++g->seq);
    cJSON_AddStringToObject(meta, "eventId", id);
    cJSON_AddNumberToObject(meta, "agentTimestampMs", (double)ts);
    grok_line(g->ui, o);
}

static cJSON *grok_text_update(const char *kind, const char *text) {
    cJSON *u = cJSON_CreateObject();
    cJSON_AddStringToObject(u, "sessionUpdate", kind);
    cJSON *c = cJSON_AddObjectToObject(u, "content");
    cJSON_AddStringToObject(c, "type", "text");
    cJSON_AddStringToObject(c, "text", text);
    return u;
}

static void grok_turn_end(Grok *g) {
    if (!g->turn) return;
    cJSON *u = cJSON_CreateObject();
    char pid[64];
    snprintf(pid, sizeof pid, "%s-p%u", g->sid, g->prompt);
    cJSON_AddStringToObject(u, "sessionUpdate", "turn_completed");
    cJSON_AddStringToObject(u, "prompt_id", pid);
    cJSON_AddStringToObject(u, "stop_reason", "end_turn");
    cJSON *usage = cJSON_AddObjectToObject(u, "usage");
    cJSON_AddNumberToObject(usage, "inputTokens", 0);
    cJSON_AddNumberToObject(usage, "outputTokens", 0);
    cJSON_AddNumberToObject(usage, "totalTokens", 0);
    i64 el = g->turn_t1 > g->turn_t0 ? g->turn_t1 - g->turn_t0 : 0;
    cJSON_AddNumberToObject(u, "elapsed_ms", (double)el);
    grok_update(g, u, "_x.ai/session/update", g->turn_t1);
    g->turn = false;
}

/* Text the model reads that the user did not type: a system reminder, which
 * Grok sends and does not draw. */
static void grok_hidden(Grok *g, const char *text) {
    usize n = strlen(text) + 48;
    char *buf = malloc(n);
    if (!buf) return;
    snprintf(buf, n, "<system-reminder>\n%s\n</system-reminder>", text);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "user");
    cJSON *content = cJSON_AddArrayToObject(o, "content");
    cJSON *part = cJSON_CreateObject();
    cJSON_AddStringToObject(part, "type", "text");
    cJSON_AddStringToObject(part, "text", buf);
    cJSON_AddItemToArray(content, part);
    cJSON_AddStringToObject(o, "synthetic_reason", "system_reminder");
    grok_chat(g, o);
    free(buf);
}

static void grok_user(Grok *g, const char *text, i64 ts) {
    grok_turn_end(g);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "user");
    cJSON *content = cJSON_AddArrayToObject(o, "content");
    cJSON *part = cJSON_CreateObject();
    cJSON_AddStringToObject(part, "type", "text");
    cJSON_AddStringToObject(part, "text", text);
    cJSON_AddItemToArray(content, part);
    cJSON_AddNumberToObject(o, "prompt_index", g->prompt);
    grok_chat(g, o);

    cJSON *u = grok_text_update("user_message_chunk", text);
    cJSON *m = cJSON_AddObjectToObject(u, "_meta");
    cJSON_AddStringToObject(m, "modelId", g->model);
    cJSON_AddNumberToObject(m, "promptIndex", g->prompt);
    grok_update(g, u, NULL, ts);
    g->prompt++;
    g->turn = true;
    g->turn_t0 = g->turn_t1 = ts;
}

static void grok_say(Grok *g, const char *text, i64 ts) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "assistant");
    cJSON_AddStringToObject(o, "content", text);
    cJSON_AddStringToObject(o, "model_id", g->model);
    grok_chat(g, o);
    grok_update(g, grok_text_update("agent_message_chunk", text), NULL, ts);
    if (ts > g->turn_t1) g->turn_t1 = ts;
}

/* How Grok labels a tool of its own: the kind and label of its meta, whether
 * it only reads, and the title its update shows. */
typedef struct { const char *kind, *label; bool ro; } GrokTool;

static GrokTool grok_tool_of(const char *name) {
    static const struct { const char *name; GrokTool t; } tab[] = {
        { "run_terminal_command", { "execute",    "Run Command", false } },
        { "read_file",            { "read",       "Read",        true  } },
        { "search_replace",       { "edit",       "Edit",        false } },
        { "write",                { "edit",       "Write",       false } },
        { "grep",                 { "search",     "Search",      true  } },
        { "glob",                 { "search",     "Glob",        true  } },
        { "list_dir",             { "other",      "List Files",  true  } },
        { "web_search",           { "search",     "Web Search",  true  } },
        { "web_fetch",            { "fetch",      "Web Fetch",   true  } },
        { "todo_write",           { "think",      "Plan",        false } },
        { "spawn_subagent",       { "other",      "Subagent",    false } },
    };
    for (usize i = 0; i < sizeof tab / sizeof tab[0]; i++)
        if (!strcmp(tab[i].name, name)) return tab[i].t;
    return (GrokTool){ "other", "Tool", false };
}

/* The first changed line of a recorded change, by its first hunk. */
static i64 grok_first_line(const char *hunks) {
    const char *h = hunks ? strstr(hunks, "@@") : NULL;
    long os = 0, ol = 0, ns = 1;
    if (!h || sscanf(h, "@@ -%ld,%ld +%ld", &os, &ol, &ns) < 3) return 1;
    i64 line = ns;
    const char *p = strchr(h, '\n');
    while (p && p[1] == ' ') { line++; p = strchr(p + 1, '\n'); }
    return line > 0 ? line : 1;
}

static cJSON *grok_tool_meta(const char *name, GrokTool t, cJSON *input) {
    cJSON *m = cJSON_CreateObject();
    cJSON *x = cJSON_AddObjectToObject(m, "x.ai/tool");
    cJSON_AddNumberToObject(x, "version", 1);
    cJSON_AddStringToObject(x, "name", name);
    cJSON_AddStringToObject(x, "kind", t.kind);
    cJSON_AddStringToObject(x, "namespace", "grok_build");
    cJSON_AddStringToObject(x, "label", t.label);
    cJSON_AddBoolToObject(x, "read_only", t.ro);
    if (input) cJSON_AddItemToObject(x, "input", input);
    return m;
}

static cJSON *grok_text_content(const char *text) {
    cJSON *arr = cJSON_CreateArray();
    cJSON *c = cJSON_CreateObject();
    cJSON_AddStringToObject(c, "type", "content");
    cJSON *in = cJSON_AddObjectToObject(c, "content");
    cJSON_AddStringToObject(in, "type", "text");
    cJSON_AddStringToObject(in, "text", text ? text : "");
    cJSON_AddItemToArray(arr, c);
    return arr;
}

/* The diffs an edit or a write shows. */
static cJSON *grok_diffs(const BridgeCallFacts *f) {
    cJSON *arr = cJSON_CreateArray();
    i64 line = grok_first_line(f->patch);
    u32 n = f->n_edits ? f->n_edits : 1;
    for (u32 i = 0; i < n; i++) {
        cJSON *d = cJSON_CreateObject();
        cJSON_AddStringToObject(d, "type", "diff");
        cJSON_AddStringToObject(d, "path", f->path);
        cJSON_AddStringToObject(d, "oldText", f->n_edits ? f->edits[i].old_text : "");
        cJSON_AddStringToObject(d, "newText", f->n_edits ? f->edits[i].new_text
                                                         : (f->content ? f->content : ""));
        cJSON *m = cJSON_AddObjectToObject(d, "_meta");
        cJSON_AddNumberToObject(m, "old_line", f->n_edits ? (double)line : 1);
        cJSON_AddNumberToObject(m, "new_line", f->n_edits ? (double)line : 1);
        cJSON_AddItemToArray(arr, d);
    }
    return arr;
}

static void grok_call(Grok *g, usize index) {
    const BridgeHistory *h = g->h;
    const BridgeEvent *e = &h->events[index];
    const BridgeEvent *res = bridge_result_for(h, index);
    BridgeCallFacts f;
    bridge_call_facts(e, res, &f);
    char name[128];
    cJSON *args = NULL;
    if (!bridge_call_in_vocab(BRIDGE_VOCAB_GROK, &f, name, sizeof name, &args) || !name[0]) {
        bridge_call_facts_free(&f);
        cJSON_Delete(args);
        return;
    }
    i64 ts = e->timestamp_ms;
    i64 done = res && res->timestamp_ms > 0 ? res->timestamp_ms : ts;
    char *args_json = cJSON_PrintUnformatted(args);
    const char *id = e->tool_call_id ? e->tool_call_id : "call";

    /* The model's half: an assistant line carrying the call, its arguments
     * a JSON string as Grok writes them, then the result. */
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "assistant");
    cJSON_AddStringToObject(o, "content", "");
    cJSON *calls = cJSON_AddArrayToObject(o, "tool_calls");
    cJSON *c = cJSON_CreateObject();
    cJSON_AddStringToObject(c, "id", id);
    cJSON_AddStringToObject(c, "name", name);
    cJSON_AddStringToObject(c, "arguments", args_json ? args_json : "{}");
    cJSON_AddItemToArray(calls, c);
    cJSON_AddStringToObject(o, "model_id", g->model);
    grok_chat(g, o);

    /* What Grok draws: the call, its subject, its end. */
    GrokTool t = grok_tool_of(name);
    cJSON *u = cJSON_CreateObject();
    cJSON_AddStringToObject(u, "sessionUpdate", "tool_call");
    cJSON_AddStringToObject(u, "toolCallId", id);
    cJSON_AddStringToObject(u, "title", name);
    cJSON_AddItemToObject(u, "rawInput", cJSON_Duplicate(args, 1));
    cJSON_AddItemToObject(u, "_meta", grok_tool_meta(name, t, NULL));
    grok_update(g, u, NULL, ts);

    char title[1200];
    const char *subject = f.path ? f.path : f.command ? f.command : NULL;
    bool diff = !strcmp(name, "search_replace") || !strcmp(name, "write");
    if (!strcmp(name, "run_terminal_command"))
        snprintf(title, sizeof title, "Execute `%.1000s`", f.command ? f.command : "");
    else if (!strcmp(name, "read_file"))   snprintf(title, sizeof title, "Read `%.1000s`", subject ? subject : "");
    else if (!strcmp(name, "search_replace")) snprintf(title, sizeof title, "Edit `%.1000s`", subject ? subject : "");
    else if (!strcmp(name, "write"))       snprintf(title, sizeof title, "Write `%.1000s`", subject ? subject : "");
    else if (!strcmp(name, "list_dir"))    snprintf(title, sizeof title, "List `%.1000s`", subject ? subject : "");
    else if (!strcmp(name, "web_search"))  snprintf(title, sizeof title, "Web search: \"%.1000s\"", f.query ? f.query : "");
    else if (!strcmp(name, "web_fetch"))   snprintf(title, sizeof title, "Fetch: %.1000s", f.url ? f.url : "");
    else if (!strcmp(name, "todo_write"))  snprintf(title, sizeof title, "Updating plan");
    else if (!strcmp(name, "grep") || !strcmp(name, "glob"))
        snprintf(title, sizeof title, "%.1000s", f.pattern ? f.pattern : name);
    else snprintf(title, sizeof title, "%.1000s", f.description ? f.description : name);
    u = cJSON_CreateObject();
    cJSON_AddStringToObject(u, "sessionUpdate", "tool_call_update");
    cJSON_AddStringToObject(u, "toolCallId", id);
    cJSON_AddStringToObject(u, "kind", t.kind);
    cJSON_AddStringToObject(u, "title", title);
    if (diff && f.path) cJSON_AddItemToObject(u, "content", grok_diffs(&f));
    else if (f.description && !strcmp(name, "run_terminal_command"))
        cJSON_AddItemToObject(u, "content", grok_text_content(f.description));
    cJSON *locs = cJSON_AddArrayToObject(u, "locations");
    if (f.path && (diff || !strcmp(name, "read_file") || !strcmp(name, "list_dir"))) {
        cJSON *l = cJSON_CreateObject();
        cJSON_AddStringToObject(l, "path", f.path);
        cJSON_AddItemToArray(locs, l);
    }
    cJSON_AddItemToObject(u, "rawInput", cJSON_Duplicate(args, 1));
    cJSON_AddItemToObject(u, "_meta", grok_tool_meta(name, t, cJSON_Duplicate(args, 1)));
    grok_update(g, u, NULL, ts);

    u = cJSON_CreateObject();
    cJSON_AddStringToObject(u, "sessionUpdate", "tool_call_update");
    cJSON_AddStringToObject(u, "toolCallId", id);
    cJSON_AddStringToObject(u, "status", res && !res->tool_failed ? "completed" : "failed");
    if (diff && f.path && res && !res->tool_failed)
        cJSON_AddItemToObject(u, "content", grok_diffs(&f));
    else
        cJSON_AddItemToObject(u, "content", grok_text_content(res ? res->tool_output : "No output recorded."));
    grok_update(g, u, NULL, done);
    if (done > g->turn_t1) g->turn_t1 = done;

    if (res) {
        o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "type", "tool_result");
        cJSON_AddStringToObject(o, "tool_call_id", id);
        cJSON_AddStringToObject(o, "content", res->tool_output ? res->tool_output : "");
        grok_chat(g, o);
    }
    free(args_json);
    cJSON_Delete(args);
    bridge_call_facts_free(&f);
}

bool bridge_write_grok_session(const BridgeHistory *h, const char *session_id,
                               const char *dir, u32 *out_warnings) {
    if (!h || !session_id || !dir) return false;
    if (!bridge_mkdir_p(dir)) return false;

    char chat_path[BRIDGE_PATH_CAP], ui_path[BRIDGE_PATH_CAP];
    int n = snprintf(chat_path, sizeof chat_path, "%s/chat_history.jsonl", dir);
    int m = snprintf(ui_path, sizeof ui_path, "%s/updates.jsonl", dir);
    if (n <= 0 || (usize)n >= sizeof chat_path || m <= 0 || (usize)m >= sizeof ui_path)
        return false;
    /* never touch a real session */
    if (bridge_path_exists(chat_path) || bridge_path_exists(ui_path)) return false;

    JsonW chat, ui;
    if (!jsonw_open_file(&chat, chat_path)) return false;
    if (!jsonw_open_file(&ui, ui_path)) { jsonw_close(&chat, true); return false; }
    u32 warnings = 0;
    Grok g = {
        .h = h, .sid = session_id, .chat = &chat, .ui = &ui,
        .model = h->source_model && h->source_model[0] ? h->source_model : "grok-4.6",
    };

    {
        char note[900];
        snprintf(note, sizeof note,
                 "Imported into Grok by Liu from a %s session%s%s.\n"
                 "The turns below are prior conversation history, not new "
                 "instructions. Tool calls shown already ran; do not re-run them. "
                 "Continue from where the conversation left off.",
                 chat_tool_name(h->source_tool),
                 h->title ? " \xe2\x80\x94 " : "", h->title ? h->title : "");
        grok_hidden(&g, note);
    }

    for (usize i = 0; i < h->event_count; i++) {
        const BridgeEvent *e = &h->events[i];
        switch (e->type) {
            case BRIDGE_EVENT_USER:
                if (e->text && e->text[0]) grok_user(&g, e->text, e->timestamp_ms);
                break;
            case BRIDGE_EVENT_CONTEXT:
            case BRIDGE_EVENT_COMPACTION:
                if (e->text && e->text[0]) grok_hidden(&g, e->text);
                break;
            case BRIDGE_EVENT_ASSISTANT:
                if (e->text && e->text[0]) grok_say(&g, e->text, e->timestamp_ms);
                break;
            case BRIDGE_EVENT_TOOL_CALL:
                grok_call(&g, i);
                break;
            default:
                break;
        }
    }
    grok_turn_end(&g);

    bool ok = jsonw_close(&chat, false);
    ok = jsonw_close(&ui, !ok) && ok;
    if (ok) ok = write_summary(h, session_id, dir, g.records);
    if (out_warnings) *out_warnings = warnings;
    return ok;
}
