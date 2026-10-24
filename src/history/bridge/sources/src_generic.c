/*
 * Shared best-effort parser for session stores represented as JSON turn
 * arrays. Recognize common role, text, and tool-record fields. These sources
 * are read for context handoff and are not written as native sessions.
 */
#include "history/bridge/sources.h"
#include "history/bridge/sources/src_jsonl.h"
#include "history/bridge/tool_map.h"
#include "history/util.h"

#include "cJSON.h"

#include <string.h>
#include <strings.h>

#define GENERIC_MAX_BYTES ((usize)MB(48))

static cJSON *obj(cJSON *o, const char *k) {
    return cJSON_GetObjectItemCaseSensitive(o, k);
}

/* First present string among `keys`. */
static const char *first_str(cJSON *o, const char *const *keys, usize n) {
    for (usize i = 0; i < n; i++) {
        const char *v = hist_cjson_str(o, keys[i]);
        if (v && v[0]) return v;
    }
    return NULL;
}

/* First present array among `keys`. */
static cJSON *first_arr(cJSON *o, const char *const *keys, usize n) {
    for (usize i = 0; i < n; i++) {
        cJSON *v = obj(o, keys[i]);
        if (v && cJSON_IsArray(v)) return v;
    }
    return NULL;
}

static BridgeEventType role_to_type(const char *role) {
    if (!role) return BRIDGE_EVENT_UNKNOWN;
    if (strcasecmp(role, "user") == 0 || strcasecmp(role, "human") == 0)
        return BRIDGE_EVENT_USER;
    if (strcasecmp(role, "assistant") == 0 || strcasecmp(role, "model") == 0 ||
        strcasecmp(role, "agent") == 0 || strcasecmp(role, "ai") == 0)
        return BRIDGE_EVENT_ASSISTANT;
    if (strcasecmp(role, "tool") == 0 || strcasecmp(role, "function") == 0)
        return BRIDGE_EVENT_TOOL_RESULT;
    if (strcasecmp(role, "system") == 0 || strcasecmp(role, "developer") == 0)
        return BRIDGE_EVENT_CONTEXT;
    return BRIDGE_EVENT_UNKNOWN;
}

static i64 turn_ts(cJSON *turn) {
    static const char *const iso_keys[] = {
        "timestamp", "createdAt", "created_at", "time", "ts", "date"
    };
    for (usize i = 0; i < sizeof iso_keys / sizeof iso_keys[0]; i++) {
        const char *s = hist_cjson_str(turn, iso_keys[i]);
        if (s && s[0]) {
            i64 t = hist_parse_iso8601_ms(s);
            if (t) return t;
        }
        i64 n = 0;
        if (hist_cjson_i64(turn, iso_keys[i], &n) && n > 0) {
            /* Seconds vs milliseconds: anything below ~2001 in ms is really
             * seconds. Getting this wrong dates a 2026 session to 1970. */
            return n < 1000000000000LL ? n * 1000 : n;
        }
    }
    return 0;
}

/* Emit one turn. `src` selects the tool-name vocabulary. */
static void push_turn(BridgeHistory *h, Arena *scratch, ChatTool src, cJSON *turn) {
    if (!turn || !cJSON_IsObject(turn)) return;

    static const char *const role_keys[] = { "role", "type", "speaker", "sender", "kind" };
    const char *role = first_str(turn, role_keys, sizeof role_keys / sizeof role_keys[0]);
    BridgeEventType et = role_to_type(role);

    i64 ts = turn_ts(turn);

    /* A tool record, in any of the spellings these lineages use. Checked
     * before the text body because several formats put a call's arguments in
     * a field the text walk would otherwise pick up as prose. */
    static const char *const tool_name_keys[] = {
        "toolName", "tool_name", "tool", "name", "function"
    };
    cJSON *tool_node = obj(turn, "toolUse");
    if (!tool_node) tool_node = obj(turn, "tool_use");
    if (!tool_node) tool_node = obj(turn, "toolCall");
    if (!tool_node) tool_node = obj(turn, "tool_call");

    if (tool_node && cJSON_IsObject(tool_node)) {
        const char *tname = first_str(tool_node, tool_name_keys,
                                      sizeof tool_name_keys / sizeof tool_name_keys[0]);
        cJSON *input = obj(tool_node, "input");
        if (!input) input = obj(tool_node, "args");
        if (!input) input = obj(tool_node, "arguments");
        if (!input) input = obj(tool_node, "parameters");
        BridgeEvent e = {0};
        e.type         = BRIDGE_EVENT_TOOL_CALL;
        e.timestamp_ms = ts;
        e.tool_name    = tname;
        e.tool_call_id = hist_cjson_str(tool_node, "id");
        e.tool_kind    = bridge_tool_kind_for(src, tname);
        e.tool_input   = bridge_json_compact(scratch, input);
        bridge_history_push(h, &e);

        cJSON *outn = obj(tool_node, "output");
        if (!outn) outn = obj(tool_node, "result");
        if (outn) {
            BridgeEvent r = {0};
            r.type         = BRIDGE_EVENT_TOOL_RESULT;
            r.timestamp_ms = ts;
            r.tool_name    = tname;
            r.tool_call_id = e.tool_call_id;
            r.tool_kind    = e.tool_kind;
            r.tool_output  = cJSON_IsString(outn) ? outn->valuestring
                                                  : hist_cjson_serialize_compact(scratch, outn);
            bridge_history_push(h, &r);
        }
        return;
    }

    /* Anthropic-shaped content array: hand it to the shared walker, which
     * already knows text / thinking / tool_use / tool_result. */
    cJSON *content = obj(turn, "content");
    if (content && cJSON_IsArray(content)) {
        cJSON *probe = cJSON_GetArrayItem(content, 0);
        if (probe && cJSON_IsObject(probe) && obj(probe, "type")) {
            bridge_push_anthropic_content(h, src, content,
                et == BRIDGE_EVENT_UNKNOWN ? BRIDGE_EVENT_USER : et, ts);
            return;
        }
    }

    /* Plain text body. */
    static const char *const text_keys[] = {
        "text", "content", "message", "body", "value", "prompt", "response"
    };
    const char *text = first_str(turn, text_keys, sizeof text_keys / sizeof text_keys[0]);
    if (!text && content && cJSON_IsObject(content))
        text = first_str(content, text_keys, sizeof text_keys / sizeof text_keys[0]);
    if (!text) return;

    /* A turn with a body but no recognisable role: attributing it to the user
     * would put words in their mouth, so it lands as CONTEXT, which every
     * target renders as background rather than as a turn. */
    if (et == BRIDGE_EVENT_UNKNOWN) et = BRIDGE_EVENT_CONTEXT;
    if (et == BRIDGE_EVENT_TOOL_RESULT) {
        bridge_push_tool_result(h, ts, NULL, NULL, text);
    } else {
        bridge_push_message(h, et, ts, text);
    }
}

/* Shared body: parse a whole-JSON session file and walk its turn array. */
static bool parse_generic(const ChatSessionMeta *m, BridgeHistory *h, ChatTool src) {
    if (!m || !m->path) return false;

    cJSON *root = (cJSON *)bridge_json_slurp(m->path, GENERIC_MAX_BYTES);
    if (!root) {
        /* Droid also ships .jsonl in some installs; the scanner accepts both
         * extensions, so a failed whole-file parse is retried line by line
         * before giving up. */
        u64 delivered = 0, bad = 0;
        (void)delivered; (void)bad;
        return false;
    }

    Arena scratch = arena_create(MB(8));
    if (!scratch.base) { cJSON_Delete(root); return false; }

    /* Metadata, wherever this format keeps it. */
    cJSON *meta = obj(root, "metadata");
    cJSON *mroot = meta && cJSON_IsObject(meta) ? meta : root;
    static const char *const cwd_keys[] = {
        "cwd", "workingDirectory", "working_directory", "workspaceRoot",
        "workspace_root", "directory", "workspace", "projectPath", "root"
    };
    static const char *const title_keys[] = { "title", "name", "summary", "topic" };
    static const char *const model_keys[] = { "model", "modelId", "modelID", "model_name" };
    static const char *const id_keys[]    = { "id", "sessionId", "session_id", "threadId", "thread_id" };

    bridge_history_set_cwd  (h, first_str(mroot, cwd_keys,   sizeof cwd_keys   / sizeof cwd_keys[0]));
    bridge_history_set_title(h, first_str(mroot, title_keys, sizeof title_keys / sizeof title_keys[0]));
    bridge_history_set_model(h, first_str(mroot, model_keys, sizeof model_keys / sizeof model_keys[0]));
    if (!h->source_session_id)
        bridge_history_set_session_id(h, first_str(mroot, id_keys, sizeof id_keys / sizeof id_keys[0]));

    /* The turn array. A bare top-level array is also accepted — the Cline
     * extension's ui_messages.json is shaped exactly that way, and several of
     * these tools' exports are too. */
    static const char *const turn_keys[] = {
        "messages", "turns", "events", "history", "conversation", "items", "entries"
    };
    cJSON *turns = cJSON_IsArray(root) ? root
                 : first_arr(root, turn_keys, sizeof turn_keys / sizeof turn_keys[0]);
    if (!turns && meta) turns = first_arr(meta, turn_keys, sizeof turn_keys / sizeof turn_keys[0]);

    if (turns) {
        cJSON *t = NULL;
        cJSON_ArrayForEach(t, turns) push_turn(h, &scratch, src, t);
    }

    arena_destroy(&scratch);
    cJSON_Delete(root);

    /* An unverified schema that produced nothing is a MISS, not an empty
     * session: reporting "no conversation" for a file that certainly has one
     * would be the wrong story. The caller turns a false into "Liu cannot
     * read that agent's session format yet". */
    return h->event_count > 0;
}

bool bridge_parse_amp(Arena *a, const ChatSessionMeta *m, BridgeHistory *h) {
    (void)a;
    return parse_generic(m, h, CHAT_TOOL_AMP);
}

bool bridge_parse_kiro(Arena *a, const ChatSessionMeta *m, BridgeHistory *h) {
    (void)a;
    return parse_generic(m, h, CHAT_TOOL_KIRO);
}

/* Droid's store carries both .json and .jsonl in the wild (the scanner
 * accepts either), so a whole-file parse that fails falls through to the line
 * driver with the same turn walker. */
typedef struct { ChatTool src; } DroidCtx;

static bool droid_line(void *root_cjson, Arena *scratch, BridgeHistory *h, void *user) {
    DroidCtx *c = user;
    push_turn(h, scratch, c->src, (cJSON *)root_cjson);
    return true;
}

bool bridge_parse_droid(Arena *a, const ChatSessionMeta *m, BridgeHistory *h) {
    (void)a;
    if (parse_generic(m, h, CHAT_TOOL_DROID)) return true;
    DroidCtx ctx = { CHAT_TOOL_DROID };
    u64 bad = 0;
    bridge_jsonl_run(m->path, h, droid_line, &ctx, &bad);
    return h->event_count > 0;
}
