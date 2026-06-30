/*
 * liu-history - Codex CLI JSONL line parser.
 *
 * Record envelope:  {timestamp, type, payload}
 * Types we care about:
 *   response_item / message          → role=user|assistant, content = [{type,text}]
 *   response_item / function_call    → CHAT_ROLE_TOOL_USE (name=payload.name,
 *                                       text=arguments)
 *   response_item / function_call_output → CHAT_ROLE_TOOL_RESULT
 *   response_item / custom_tool_call / custom_tool_call_output  → same as above
 *   response_item / reasoning        → CHAT_ROLE_SYSTEM with the summary text
 *
 * Everything else (session_meta, turn_context, event_msg, etc.) skipped.
 */
#include "history/parser.h"
#include "history/event.h"
#include "history/util.h"

#include "cJSON.h"

#include <string.h>

#define cstr hist_cjson_str

static char *flatten_message_content(Arena *a, cJSON *content) {
    if (!content || !cJSON_IsArray(content)) return NULL;
    usize total = 0;
    cJSON *it;
    cJSON_ArrayForEach(it, content) {
        const char *tp = cstr(it, "type");
        const char *tx = cstr(it, "text");
        if (!tx) continue;
        if (!tp || (strcmp(tp, "input_text") && strcmp(tp, "output_text") &&
                    strcmp(tp, "text"))) continue;
        total += strlen(tx) + 1;
    }
    if (!total) return NULL;
    char *buf = arena_alloc(a, total + 1);
    if (!buf) return NULL;
    usize off = 0;
    cJSON_ArrayForEach(it, content) {
        const char *tp = cstr(it, "type");
        const char *tx = cstr(it, "text");
        if (!tx) continue;
        if (!tp || (strcmp(tp, "input_text") && strcmp(tp, "output_text") &&
                    strcmp(tp, "text"))) continue;
        usize n = strlen(tx);
        if (off > 0 && off < total) buf[off++] = '\n';
        memcpy(buf + off, tx, n); off += n;
    }
    buf[off] = '\0';
    return buf;
}

u32 parser_codex_line(Arena *a, const char *line, usize len,
                      ChatEvent *out, u32 out_cap) {
    if (!line || !len || !out || !out_cap) return 0;
    if (len > 4 * 1024 * 1024) return 0;

    cJSON *root = cJSON_ParseWithLength(line, len);
    if (!root) return 0;

    u32 emitted = 0;
    const char *env_type = cstr(root, "type");
    const char *ts       = cstr(root, "timestamp");
    i64 t_ms = hist_parse_iso8601_ms(ts);

    /* session_id not in individual records — caller will tag via path. */
    if (!env_type) goto done;

    if (strcmp(env_type, "response_item") != 0) goto done;

    cJSON *payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
    if (!payload || !cJSON_IsObject(payload)) goto done;
    const char *ptype = cstr(payload, "type");
    if (!ptype) goto done;

    if (strcmp(ptype, "message") == 0) {
        const char *role = cstr(payload, "role");
        ChatRole r = CHAT_ROLE_SYSTEM;
        if (role && strcmp(role, "user") == 0)       r = CHAT_ROLE_USER;
        else if (role && strcmp(role, "assistant") == 0) r = CHAT_ROLE_ASSISTANT;
        cJSON *content = cJSON_GetObjectItemCaseSensitive(payload, "content");
        if (emitted < out_cap) {
            out[emitted++] = (ChatEvent){
                .origin_tool = CHAT_TOOL_CODEX,
                .role        = r,
                .timestamp_ms= t_ms,
                .text        = flatten_message_content(a, content),
                .tool_name   = NULL,
                .session_id  = NULL,
            };
        }
    } else if (strcmp(ptype, "function_call") == 0 ||
               strcmp(ptype, "custom_tool_call") == 0) {
        if (emitted < out_cap) {
            const char *name = cstr(payload, "name");
            const char *args = cstr(payload, "arguments");
            if (!args) args = cstr(payload, "input");
            out[emitted++] = (ChatEvent){
                .origin_tool = CHAT_TOOL_CODEX,
                .role        = CHAT_ROLE_TOOL_USE,
                .timestamp_ms= t_ms,
                .text        = hist_strdup(a, args),
                .tool_name   = hist_strdup(a, name),
                .session_id  = NULL,
            };
        }
    } else if (strcmp(ptype, "function_call_output") == 0 ||
               strcmp(ptype, "custom_tool_call_output") == 0) {
        if (emitted < out_cap) {
            const char *output = cstr(payload, "output");
            const char *call_id = cstr(payload, "call_id");
            out[emitted++] = (ChatEvent){
                .origin_tool = CHAT_TOOL_CODEX,
                .role        = CHAT_ROLE_TOOL_RESULT,
                .timestamp_ms= t_ms,
                .text        = hist_strdup(a, output),
                .tool_name   = hist_strdup(a, call_id),
                .session_id  = NULL,
            };
        }
    } else if (strcmp(ptype, "reasoning") == 0) {
        /* Flatten the summary[].text array into one system event. */
        cJSON *summary = cJSON_GetObjectItemCaseSensitive(payload, "summary");
        char *txt = flatten_message_content(a, summary);
        if (txt && emitted < out_cap) {
            out[emitted++] = (ChatEvent){
                .origin_tool = CHAT_TOOL_CODEX,
                .role        = CHAT_ROLE_SYSTEM,
                .timestamp_ms= t_ms,
                .text        = txt,
                .tool_name   = "reasoning",
                .session_id  = NULL,
            };
        }
    }

done:
    cJSON_Delete(root);
    return emitted;
}
