/*
 * Bridge-native source parser for Command Code sessions:
 *   ~/.commandcode/projects/<slug>/<uuid>.jsonl
 *
 * As Command Code 1.88 writes them: a {"type":"session","cwd",…} header, then
 * {"type":"message","id","parentId","timestamp","message":{"role":"user"|
 * "assistant","content":[…]}} records whose content blocks are Anthropic's
 * (text, thinking, tool_use, tool_result, image), and {"type":"compaction",
 * "summary",…} records. A message marked isMeta (on the record's metadata or
 * the message's meta) is text Command Code sent and did not draw: context,
 * not a turn.
 *
 * The earlier flat layout ({"role",…,"content":[tool-call …]}) is read by the
 * ChatEvent fold; this parser reports nothing for it.
 */
#include "history/bridge/sources.h"
#include "history/bridge/sources/src_jsonl.h"
#include "history/util.h"

#include "cJSON.h"

#include <string.h>

typedef struct { bool saw_cwd; } CcCtx;

static bool cc_meta(cJSON *root, cJSON *msg) {
    cJSON *md = cJSON_GetObjectItemCaseSensitive(root, "metadata");
    cJSON *mm = msg ? cJSON_GetObjectItemCaseSensitive(msg, "meta") : NULL;
    return (md && hist_cjson_bool(md, "isMeta")) || (mm && hist_cjson_bool(mm, "isMeta"));
}

static bool cc_line(void *root_cjson, Arena *scratch, BridgeHistory *h, void *user) {
    (void)scratch;
    cJSON *root = (cJSON *)root_cjson;
    CcCtx *ctx = user;
    const char *type = hist_cjson_str(root, "type");
    if (!type) return true;
    i64 ts = hist_parse_iso8601_ms(hist_cjson_str(root, "timestamp"));

    if (!strcmp(type, "session")) {
        if (!ctx->saw_cwd) {
            bridge_history_set_cwd(h, hist_cjson_str(root, "cwd"));
            bridge_history_set_session_id(h, hist_cjson_str(root, "id"));
            ctx->saw_cwd = h->cwd != NULL;
        }
        return true;
    }
    if (!strcmp(type, "compaction")) {
        bridge_push_message(h, BRIDGE_EVENT_COMPACTION, ts, hist_cjson_str(root, "summary"));
        return true;
    }
    if (strcmp(type, "message") != 0) return true;

    cJSON *msg = cJSON_GetObjectItemCaseSensitive(root, "message");
    const char *role = msg ? hist_cjson_str(msg, "role") : NULL;
    cJSON *content = msg ? cJSON_GetObjectItemCaseSensitive(msg, "content") : NULL;
    if (!role || !content) return true;
    bool user_side = !strcmp(role, "user");
    if (!user_side && strcmp(role, "assistant") != 0) return true;
    if (!user_side && !h->source_model) bridge_history_set_model(h, hist_cjson_str(root, "model"));

    BridgeEventType et = user_side ? BRIDGE_EVENT_USER : BRIDGE_EVENT_ASSISTANT;
    if (user_side && cc_meta(root, msg)) et = BRIDGE_EVENT_CONTEXT;
    bridge_push_anthropic_content(h, CHAT_TOOL_COMMANDCODE, content, et, ts);
    return true;
}

bool bridge_parse_commandcode(Arena *a, const ChatSessionMeta *m, BridgeHistory *h) {
    (void)a;
    if (!m || !m->path) return false;
    CcCtx ctx = {0};
    u64 bad = 0;
    bridge_jsonl_run(m->path, h, cc_line, &ctx, &bad);
    if (bad > 0) {
        h->fidelity.events_dropped += (u32)(bad > 0xFFFFFFFFu ? 0xFFFFFFFFu : bad);
        h->fidelity.history_complete = false;
    }
    return h->event_count > 0;
}
