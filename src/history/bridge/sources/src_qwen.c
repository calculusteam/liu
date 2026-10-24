/*
 * liu-history/bridge - Qwen Code source parser (bridge-native).
 *
 * Store: ~/.qwen/projects/<slugged-cwd>/chats/<session>.jsonl
 * ($QWEN_RUNTIME_DIR / $QWEN_HOME override the root, in that precedence).
 *
 * Qwen Code is a gemini-cli fork, so its chat records are Gemini `Content`
 * objects rather than OpenAI or Anthropic ones. That shape is the whole
 * reason this parser is not a two-line alias of another:
 *
 *   {"role":"user",  "parts":[{"text":"…"}]}
 *   {"role":"model", "parts":[{"text":"…"},
 *                             {"functionCall":{"name":…,"args":{…},"id":…}}]}
 *   {"role":"user",  "parts":[{"functionResponse":{"name":…,"id":…,
 *                                                  "response":{…}}}]}
 *
 * Three Gemini-isms, each of which silently produces the wrong transcript if
 * the OpenAI shape is assumed:
 *
 *   - the assistant role is spelled `model`, not `assistant`;
 *   - a tool RESULT arrives on the **user** role (Gemini models function
 *     responses as user-supplied), so classifying by role alone turns every
 *     tool output into a user turn;
 *   - `thought` / `thoughtSignature` parts are the thinking channel and are
 *     dropped like every other source's.
 *
 * The shape comes from gemini-cli's own Content schema rather than from an
 * observed store, so the target side is gated accordingly — see
 * compatibility.c.
 */
#include "history/bridge/sources.h"
#include "history/bridge/sources/src_jsonl.h"
#include "history/bridge/tool_map.h"
#include "history/util.h"

#include "cJSON.h"

#include <string.h>

static bool qwen_line(void *root_cjson, Arena *scratch, BridgeHistory *h,
                      void *user) {
    (void)user;
    cJSON *root = (cJSON *)root_cjson;

    /* Some builds wrap each turn in an envelope with a timestamp and put the
     * Content under "content" or "message"; others write the Content bare. */
    cJSON *content = cJSON_GetObjectItemCaseSensitive(root, "content");
    if (!content) content = cJSON_GetObjectItemCaseSensitive(root, "message");
    cJSON *node = content && cJSON_IsObject(content) ? content : root;

    i64 ts = 0;
    const char *iso = hist_cjson_str(root, "timestamp");
    if (iso) ts = hist_parse_iso8601_ms(iso);
    if (!ts) (void)hist_cjson_i64(root, "timestamp", &ts);

    const char *role = hist_cjson_str(node, "role");
    cJSON *parts = cJSON_GetObjectItemCaseSensitive(node, "parts");
    if (!parts || !cJSON_IsArray(parts)) return true;

    bool is_model = role && strcmp(role, "model") == 0;

    cJSON *part = NULL;
    cJSON_ArrayForEach(part, parts) {
        /* Thinking channel. `thought` is a bool flag on a text part in newer
         * builds and a part type in older ones; both are skipped. */
        if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(part, "thought")))
            continue;
        if (cJSON_GetObjectItemCaseSensitive(part, "thoughtSignature")) continue;

        cJSON *fc = cJSON_GetObjectItemCaseSensitive(part, "functionCall");
        if (fc) {
            const char *name = hist_cjson_str(fc, "name");
            cJSON *args = cJSON_GetObjectItemCaseSensitive(fc, "args");
            BridgeEvent e = {0};
            e.type         = BRIDGE_EVENT_TOOL_CALL;
            e.timestamp_ms = ts;
            e.tool_name    = name;
            e.tool_call_id = hist_cjson_str(fc, "id");
            e.tool_kind    = bridge_tool_kind_for(CHAT_TOOL_QWEN, name);
            e.tool_input   = bridge_json_compact(scratch, args);
            bridge_history_push(h, &e);
            continue;
        }

        cJSON *fr = cJSON_GetObjectItemCaseSensitive(part, "functionResponse");
        if (fr) {
            const char *name = hist_cjson_str(fr, "name");
            cJSON *resp = cJSON_GetObjectItemCaseSensitive(fr, "response");
            /* The response object usually wraps the real payload under
             * "output" or "result"; anything else is serialized whole. */
            const char *out = resp ? hist_cjson_str(resp, "output") : NULL;
            if (!out && resp) out = hist_cjson_str(resp, "result");
            if (!out && resp) out = hist_cjson_serialize_compact(scratch, resp);
            BridgeEvent e = {0};
            e.type         = BRIDGE_EVENT_TOOL_RESULT;
            e.timestamp_ms = ts;
            e.tool_name    = name;
            e.tool_call_id = hist_cjson_str(fr, "id");
            e.tool_kind    = bridge_tool_kind_for(CHAT_TOOL_QWEN, name);
            e.tool_output  = out;
            bridge_history_push(h, &e);
            continue;
        }

        const char *tx = hist_cjson_str(part, "text");
        if (tx && tx[0]) {
            bridge_push_message(h, is_model ? BRIDGE_EVENT_ASSISTANT
                                            : BRIDGE_EVENT_USER, ts, tx);
        }
    }
    return true;
}

bool bridge_parse_qwen(Arena *a, const ChatSessionMeta *m, BridgeHistory *h) {
    (void)a;
    if (!m || !m->path) return false;
    /* The scanner already decodes Qwen's project slug (same '/' -> '-'
     * convention Claude uses) into meta->cwd, so nothing extra is needed. */
    u64 bad = 0;
    bridge_jsonl_run(m->path, h, qwen_line, NULL, &bad);
    if (bad > 0) {
        h->fidelity.events_dropped += (u32)(bad > 0xFFFFFFFFu ? 0xFFFFFFFFu : bad);
        h->fidelity.history_complete = false;
    }
    return h->event_count > 0;
}
