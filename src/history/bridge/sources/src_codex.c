/*
 * Bridge-native source parser for Codex CLI rollout JSONL. Read the record
 * envelope and preserve message roles, content, and tool-call data in the
 * bridge history.
 *
 * A rollout that records its tool calls as items (`item_completed` with a
 * CommandExecution, FileChange or McpToolCall) is read from those: they are
 * what Codex itself shows — the command, its output and exit code, the diff
 * of a change — while the model-side calls behind them can be a code-mode
 * `exec` script that names none of it. The model-side calls of the tools that
 * produce those items are then skipped, so no call is read twice.
 */
#include "history/bridge/sources.h"
#include "history/bridge/sources/src_jsonl.h"
#include "history/bridge/tool_map.h"
#include "history/util.h"

#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *flatten_content(Arena *a, cJSON *content, u32 *media);

typedef struct {
    bool   items;     /* the rollout records its tool calls as items        */
    char **skip;      /* call ids whose outputs are skipped with their call */
    usize  n_skip, cap_skip;
} CodexCtx;

static void ctx_skip_add(CodexCtx *c, BridgeHistory *h, const char *id) {
    if (c->n_skip == c->cap_skip) {
        usize ncap = c->cap_skip ? c->cap_skip * 2 : 64;
        char **q = realloc(c->skip, ncap * sizeof *q);
        if (!q) return;
        c->skip = q;
        c->cap_skip = ncap;
    }
    usize n = strlen(id);
    char *copy = arena_alloc(h->arena, n + 1);
    if (!copy) return;
    memcpy(copy, id, n + 1);
    c->skip[c->n_skip++] = copy;
}

static bool ctx_skipped(const CodexCtx *c, const char *id) {
    if (!id) return false;
    for (usize i = c->n_skip; i-- > 0;) if (strcmp(c->skip[i], id) == 0) return true;
    return false;
}

/* Tools whose calls a rollout with items records again as an item. */
static bool codex_item_tool(const char *name) {
    static const char *const names[] = {
        "exec", "shell", "exec_command", "local_shell", "shell_command", "apply_patch",
        "write_stdin", "container.exec", "unified_exec",
    };
    if (!name) return false;
    for (usize i = 0; i < sizeof names / sizeof names[0]; i++)
        if (strcmp(name, names[i]) == 0) return true;
    return false;
}

/* Whether the file at `path` records CommandExecution or FileChange items:
 * one pass over its bytes, a pattern straddling two reads included. */
static bool codex_has_items(const char *path) {
    static const char *const pats[] = {
        "\"type\":\"CommandExecution\"", "\"type\": \"CommandExecution\"",
        "\"type\":\"FileChange\"",       "\"type\": \"FileChange\"",
    };
    FILE *fp = hist_fopen_regular(path, true);
    if (!fp) return false;
    char buf[64 * 1024 + 64];
    usize keep = 0;
    bool found = false;
    for (;;) {
        usize n = fread(buf + keep, 1, sizeof buf - 1 - keep, fp);
        if (n == 0) break;
        usize len = keep + n;
        buf[len] = '\0';
        for (usize i = 0; i < sizeof pats / sizeof pats[0] && !found; i++)
            if (strstr(buf, pats[i])) found = true;
        if (found) break;
        keep = len < 48 ? len : 48;
        memmove(buf, buf + len - keep, keep);
    }
    fclose(fp);
    return found;
}

static const char *json_compact(Arena *a, cJSON *o) {
    return o ? hist_cjson_serialize_compact(a, o) : NULL;
}

/* A CommandExecution item as the call it was: a read or a search when Codex
 * parsed the command as one, a shell command otherwise. */
static void codex_item_command(Arena *a, BridgeHistory *h, cJSON *it, i64 ts) {
    const char *id = hist_cjson_str(it, "id");
    cJSON *argv = cJSON_GetObjectItemCaseSensitive(it, "command");
    cJSON *parsed = cJSON_GetObjectItemCaseSensitive(it, "parsed_cmd");
    cJSON *one = cJSON_GetArraySize(parsed) == 1 ? cJSON_GetArrayItem(parsed, 0) : NULL;
    const char *pt = one ? hist_cjson_str(one, "type") : NULL;
    cJSON *in = cJSON_CreateObject();
    if (!in) return;
    BridgeToolKind kind = BRIDGE_TOOL_SHELL;
    const char *name = "shell";
    if (pt && !strcmp(pt, "read") && hist_cjson_str(one, "path")) {
        kind = BRIDGE_TOOL_FILE_READ;
        name = "read";
        cJSON_AddStringToObject(in, "file_path", hist_cjson_str(one, "path"));
    } else if (pt && !strcmp(pt, "search") && hist_cjson_str(one, "query")) {
        kind = BRIDGE_TOOL_TEXT_SEARCH;
        name = "search";
        cJSON_AddStringToObject(in, "pattern", hist_cjson_str(one, "query"));
        if (hist_cjson_str(one, "path")) cJSON_AddStringToObject(in, "path", hist_cjson_str(one, "path"));
    } else if (argv) {
        /* A listing stays the command it was: agents draw `ls` as one. */
        cJSON_AddItemToObject(in, "command", cJSON_Duplicate(argv, 1));
    }
    BridgeEvent c = {0};
    c.type = BRIDGE_EVENT_TOOL_CALL;
    c.timestamp_ms = ts;
    c.tool_name = name;
    c.tool_call_id = id;
    c.tool_kind = kind;
    c.tool_input = json_compact(a, in);
    cJSON_Delete(in);
    if (!bridge_history_push(h, &c)) return;

    i64 code = 0;
    const char *status = hist_cjson_str(it, "status");
    BridgeEvent r = {0};
    r.type = BRIDGE_EVENT_TOOL_RESULT;
    r.timestamp_ms = ts;
    r.tool_call_id = id;
    r.tool_output = hist_cjson_str(it, "aggregated_output");
    r.tool_failed = (hist_cjson_i64(it, "exit_code", &code) && code != 0) ||
                    (status && strcmp(status, "completed") != 0);
    if (!r.tool_output) r.tool_output = r.tool_failed ? "failed" : "done";
    bridge_history_push(h, &r);
}

/* A FileChange item: one call per file, an update with the diff Codex
 * recorded, an addition with its content. */
static void codex_item_files(Arena *a, BridgeHistory *h, cJSON *it, i64 ts) {
    const char *id = hist_cjson_str(it, "id");
    cJSON *changes = cJSON_GetObjectItemCaseSensitive(it, "changes");
    const char *status = hist_cjson_str(it, "status");
    bool failed = status && strcmp(status, "completed") != 0;
    int n = 0;
    cJSON *ch = NULL;
    cJSON_ArrayForEach(ch, changes) {
        const char *path = ch->string;
        const char *type = hist_cjson_str(ch, "type");
        if (!path || !type || !strcmp(type, "delete")) continue;
        bool add = !strcmp(type, "add");
        char cid[160];
        snprintf(cid, sizeof cid, "%.140s#%d", id ? id : "change", n++);
        cJSON *in = cJSON_CreateObject();
        if (!in) return;
        cJSON_AddStringToObject(in, "file_path", path);
        if (add && hist_cjson_str(ch, "content")) cJSON_AddStringToObject(in, "content", hist_cjson_str(ch, "content"));
        BridgeEvent c = {0};
        c.type = BRIDGE_EVENT_TOOL_CALL;
        c.timestamp_ms = ts;
        c.tool_name = add ? "write" : "apply_patch";
        c.tool_call_id = cid;
        c.tool_kind = add ? BRIDGE_TOOL_FILE_WRITE : BRIDGE_TOOL_FILE_PATCH;
        c.tool_input = json_compact(a, in);
        cJSON_Delete(in);
        if (!bridge_history_push(h, &c)) continue;
        BridgeEvent r = {0};
        r.type = BRIDGE_EVENT_TOOL_RESULT;
        r.timestamp_ms = ts;
        r.tool_call_id = cid;
        r.tool_output = failed ? "The change was not applied." : (add ? "Created." : "Updated.");
        r.tool_failed = failed;
        if (bridge_history_push(h, &r))
            bridge_history_set_file_change(h, h->event_count - 1,
                                           add ? NULL : hist_cjson_str(ch, "unified_diff"), add);
    }
}

static void codex_item_mcp(Arena *a, BridgeHistory *h, cJSON *it, i64 ts) {
    const char *server = hist_cjson_str(it, "server"), *tool = hist_cjson_str(it, "tool");
    if (!server || !tool) return;
    char name[256];
    snprintf(name, sizeof name, "mcp__%s__%s", server, tool);
    BridgeEvent c = {0};
    c.type = BRIDGE_EVENT_TOOL_CALL;
    c.timestamp_ms = ts;
    c.tool_name = name;
    c.tool_call_id = hist_cjson_str(it, "id");
    c.tool_kind = BRIDGE_TOOL_MCP;
    c.tool_input = json_compact(a, cJSON_GetObjectItemCaseSensitive(it, "arguments"));
    if (!bridge_history_push(h, &c)) return;
    cJSON *res = cJSON_GetObjectItemCaseSensitive(it, "result");
    cJSON *content = res ? cJSON_GetObjectItemCaseSensitive(res, "content") : NULL;
    u32 media = 0;
    BridgeEvent r = {0};
    r.type = BRIDGE_EVENT_TOOL_RESULT;
    r.timestamp_ms = ts;
    r.tool_call_id = c.tool_call_id;
    r.tool_output = flatten_content(a, content, &media);
    if (!r.tool_output) r.tool_output = json_compact(a, res);
    r.tool_failed = res && hist_cjson_bool(res, "isError");
    bridge_history_push(h, &r);
}

/* The text of one part of a Responses content array, in every spelling Codex
 * uses across versions — input_text (user), output_text (assistant), text —
 * or, for an input_image, the note that stands for it (written to `note`,
 * counted in `*media`). NULL for anything else. */
static const char *part_text(cJSON *it, char *note, usize note_cap, u32 *media) {
    const char *tp = hist_cjson_str(it, "type");
    if (!tp) return NULL;
    if (!strcmp(tp, "input_text") || !strcmp(tp, "output_text") || !strcmp(tp, "text"))
        return hist_cjson_str(it, "text");
    if (bridge_media_note(it, note, note_cap)) {
        if (media) (*media)++;
        return note;
    }
    return NULL;
}

/* Join a Responses content array into one text. An image the user attached,
 * or one a tool returned, is a data: URL of base64 here: it becomes its note
 * (`*media` says how many did), because the payload cannot go to another
 * agent, and a turn that was only an image is still counted instead of
 * vanishing. */
static const char *flatten_content(Arena *a, cJSON *content, u32 *media) {
    *media = 0;
    if (!content) return NULL;
    if (cJSON_IsString(content))
        return content->valuestring && content->valuestring[0] ? content->valuestring : NULL;
    if (!cJSON_IsArray(content)) return NULL;

    char note[256];
    usize total = 0;
    cJSON *it = NULL;
    cJSON_ArrayForEach(it, content) {
        const char *tx = part_text(it, note, sizeof note, NULL);
        if (tx) total += strlen(tx) + 1;
    }
    if (!total) return NULL;
    char *buf = arena_alloc(a, total + 1);
    if (!buf) return NULL;
    usize off = 0;
    cJSON_ArrayForEach(it, content) {
        const char *tx = part_text(it, note, sizeof note, media);
        if (!tx) continue;
        usize n = strlen(tx);
        if (off > 0 && off < total) buf[off++] = '\n';
        memcpy(buf + off, tx, n);
        off += n;
    }
    buf[off] = '\0';
    return buf;
}

static bool codex_line(void *root_cjson, Arena *scratch, BridgeHistory *h,
                       void *user) {
    CodexCtx *ctx = user;
    cJSON *root = (cJSON *)root_cjson;

    const char *env = hist_cjson_str(root, "type");
    if (!env) return true;
    i64 ts = hist_parse_iso8601_ms(hist_cjson_str(root, "timestamp"));

    cJSON *payload = cJSON_GetObjectItemCaseSensitive(root, "payload");

    if (strcmp(env, "session_meta") == 0) {
        if (payload) {
            bridge_history_set_session_id(h, hist_cjson_str(payload, "id"));
            if (!h->source_session_id)
                bridge_history_set_session_id(h, hist_cjson_str(payload, "session_id"));
            bridge_history_set_cwd(h, hist_cjson_str(payload, "cwd"));
            /* `model` is not on session_meta in every version; turn_context
             * carries it when it is missing here. */
            bridge_history_set_model(h, hist_cjson_str(payload, "model"));
            cJSON *git = cJSON_GetObjectItemCaseSensitive(payload, "git");
            if (git) {
                bridge_history_set_git(h,
                    hist_cjson_str(git, "repository_url"),
                    hist_cjson_str(git, "branch"),
                    hist_cjson_str(git, "commit_hash"));
            }
        }
        return true;
    }

    if (strcmp(env, "turn_context") == 0) {
        if (payload) {
            if (!h->cwd)          bridge_history_set_cwd(h, hist_cjson_str(payload, "cwd"));
            if (!h->source_model) bridge_history_set_model(h, hist_cjson_str(payload, "model"));
        }
        return true;
    }

    if (ctx && ctx->items && strcmp(env, "event_msg") == 0 && payload) {
        const char *et = hist_cjson_str(payload, "type");
        cJSON *it = et && !strcmp(et, "item_completed")
                        ? cJSON_GetObjectItemCaseSensitive(payload, "item") : NULL;
        const char *k = it ? hist_cjson_str(it, "type") : NULL;
        if (k && !strcmp(k, "CommandExecution")) codex_item_command(h->arena, h, it, ts);
        else if (k && !strcmp(k, "FileChange")) codex_item_files(h->arena, h, it, ts);
        else if (k && !strcmp(k, "McpToolCall")) codex_item_mcp(h->arena, h, it, ts);
        return true;
    }

    if (strcmp(env, "response_item") != 0) return true;
    if (!payload || !cJSON_IsObject(payload)) return true;

    const char *pt = hist_cjson_str(payload, "type");
    if (!pt) return true;

    if (strcmp(pt, "message") == 0) {
        const char *role = hist_cjson_str(payload, "role");
        cJSON *content = cJSON_GetObjectItemCaseSensitive(payload, "content");
        u32 media = 0;
        const char *text = flatten_content(scratch, content, &media);
        if (!text) return true;
        if (media) {
            h->fidelity.events_dropped += media;
            h->fidelity.history_complete = false;
        }

        BridgeEventType et = BRIDGE_EVENT_SYSTEM;
        if (role && strcmp(role, "user") == 0)            et = BRIDGE_EVENT_USER;
        else if (role && strcmp(role, "assistant") == 0)  et = BRIDGE_EVENT_ASSISTANT;
        else if (role && strcmp(role, "developer") == 0)  et = BRIDGE_EVENT_CONTEXT;

        /* Codex injects two things at thread start on the USER role that the
         * user did not type: the environment blob, wrapped in
         * <environment_context>, and the project's AGENTS.md under a literal
         * "# AGENTS.md instructions for <path>" heading. Both are the
         * harness, and both are regenerated by whatever agent runs next, so
         * replaying them as user turns is wrong twice — it shows the next
         * model a message the user never sent, and in a CHAINED migration
         * (Claude -> Codex -> …) it accumulates one synthetic user turn per
         * hop. */
        if (et == BRIDGE_EVENT_USER && text &&
            (strncmp(text, "<environment_context>", 21) == 0 ||
             strncmp(text, "# AGENTS.md instructions for ", 29) == 0)) {
            et = BRIDGE_EVENT_CONTEXT;
        }
        bridge_push_message(h, et, ts, text);
        return true;
    }

    if (strcmp(pt, "function_call") == 0 || strcmp(pt, "custom_tool_call") == 0) {
        const char *name = hist_cjson_str(payload, "name");
        if (ctx && ctx->items && codex_item_tool(name)) {
            /* Its item carries the call; its output is skipped with it. */
            const char *cid = hist_cjson_str(payload, "call_id");
            if (cid) ctx_skip_add(ctx, h, cid);
            return true;
        }
        const char *args = hist_cjson_str(payload, "arguments");
        if (!args) args = hist_cjson_str(payload, "input");
        BridgeEvent e = {0};
        e.type         = BRIDGE_EVENT_TOOL_CALL;
        e.timestamp_ms = ts;
        e.tool_name    = name;
        e.tool_call_id = hist_cjson_str(payload, "call_id");
        e.tool_kind    = bridge_tool_kind_for(CHAT_TOOL_CODEX, name);
        e.tool_input   = args;
        bridge_history_push(h, &e);
        return true;
    }

    if (strcmp(pt, "function_call_output") == 0 ||
        strcmp(pt, "custom_tool_call_output") == 0) {
        if (ctx && ctx_skipped(ctx, hist_cjson_str(payload, "call_id"))) return true;
        cJSON *outv = cJSON_GetObjectItemCaseSensitive(payload, "output");
        const char *out = NULL;
        u32 media = 0;
        if (cJSON_IsString(outv)) out = outv->valuestring;
        /* A list of content parts (view_image answers with one): its text
         * and the notes for its images, not the JSON around a data: URL. */
        else if (cJSON_IsArray(outv)) out = flatten_content(scratch, outv, &media);
        if (!out && outv && !cJSON_IsString(outv))
            out = hist_cjson_serialize_compact(scratch, outv);
        BridgeEvent e = {0};
        e.type         = BRIDGE_EVENT_TOOL_RESULT;
        e.timestamp_ms = ts;
        e.tool_call_id = hist_cjson_str(payload, "call_id");
        e.tool_output  = out;
        e.tool_output_truncated = media > 0;
        bridge_history_push(h, &e);
        return true;
    }

    /* `reasoning` carries a human-readable summary AND `encrypted_content`.
     * Neither is imported: the summary is model-private scratch that the
     * origin tool renders in a separate channel, and the encrypted blob is
     * bound to the provider's key and meaningless anywhere else. */
    return true;
}

bool bridge_parse_codex(Arena *a, const ChatSessionMeta *m, BridgeHistory *h) {
    (void)a;
    if (!m || !m->path) return false;
    u64 bad = 0;
    CodexCtx ctx = { .items = codex_has_items(m->path) };
    bridge_jsonl_run(m->path, h, codex_line, &ctx, &bad);
    free(ctx.skip);
    if (bad > 0) {
        h->fidelity.events_dropped += (u32)(bad > 0xFFFFFFFFu ? 0xFFFFFFFFu : bad);
        h->fidelity.history_complete = false;
    }
    return h->event_count > 0;
}
