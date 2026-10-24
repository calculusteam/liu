/*
 * Bridge-native source parser for Codex CLI rollout JSONL. Read the record
 * envelope and preserve message roles, content, and tool-call data in the
 * bridge history.
 */
#include "history/bridge/sources.h"
#include "history/bridge/sources/src_jsonl.h"
#include "history/bridge/tool_map.h"
#include "history/util.h"

#include "cJSON.h"

#include <string.h>

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
    (void)user;
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
    bridge_jsonl_run(m->path, h, codex_line, NULL, &bad);
    if (bad > 0) {
        h->fidelity.events_dropped += (u32)(bad > 0xFFFFFFFFu ? 0xFFFFFFFFu : bad);
        h->fidelity.history_complete = false;
    }
    return h->event_count > 0;
}
