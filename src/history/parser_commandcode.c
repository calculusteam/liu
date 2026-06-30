/*
 * liu-history - CommandCode session .jsonl line parser.
 *
 * Store: ~/.commandcode/projects/<slug>/<uuid>.jsonl (siblings: .meta.json,
 * .checkpoints.jsonl — both excluded by the scanner). One JSON object per
 * line:
 *   {"id","timestamp":"ISO8601","sessionId","parentId",
 *    "role":"user"|"assistant"|"tool",
 *    "content":"…" | [{"type":"text","text":…} |
 *                     {"type":"tool-call","toolCallId","toolName","input":{…}} |
 *                     {"type":"tool-result","toolCallId","toolName","output":…} |
 *                     {"type":"reasoning"…} | {"type":"image"…}], …}
 *
 * "reasoning" (thinking channel) and "image" blocks are skipped; text blocks
 * map to the line's role, tool-call → TOOL_USE, tool-result → TOOL_RESULT.
 */
#include "history/parser.h"
#include "history/event.h"
#include "history/util.h"

#include "cJSON.h"

#include <string.h>

static ChatRole cc_role(const char *role) {
    if (role && strcmp(role, "user") == 0)      return CHAT_ROLE_USER;
    if (role && strcmp(role, "assistant") == 0) return CHAT_ROLE_ASSISTANT;
    return CHAT_ROLE_TOOL_RESULT;   /* "tool" lines carry results */
}

u32 parser_commandcode_line(Arena *a, const char *line, usize len,
                            ChatEvent *out, u32 out_cap) {
    if (!line || !len || !out || !out_cap) return 0;
    if (len > 4 * 1024 * 1024) return 0;  /* paranoia cap */

    cJSON *root = cJSON_ParseWithLength(line, len);
    if (!root) return 0;

    u32 emitted = 0;
    const char *role = hist_cjson_str(root, "role");
    if (!role) goto done;

    i64 t_ms = hist_parse_iso8601_ms(hist_cjson_str(root, "timestamp"));
    char *sid_own = hist_strdup(a, hist_cjson_str(root, "sessionId"));
    ChatRole text_role = cc_role(role);

    cJSON *content = cJSON_GetObjectItemCaseSensitive(root, "content");
    if (cJSON_IsString(content)) {
        if (content->valuestring && content->valuestring[0] && emitted < out_cap) {
            out[emitted++] = (ChatEvent){
                .origin_tool = CHAT_TOOL_COMMANDCODE,
                .role        = text_role,
                .timestamp_ms= t_ms,
                .text        = hist_strdup(a, content->valuestring),
                .tool_name   = NULL,
                .session_id  = sid_own,
            };
        }
        goto done;
    }
    if (!cJSON_IsArray(content)) goto done;

    cJSON *blk = NULL;
    cJSON_ArrayForEach(blk, content) {
        if (emitted >= out_cap) break;
        const char *bt = hist_cjson_str(blk, "type");
        if (!bt) continue;

        if (strcmp(bt, "text") == 0) {
            const char *tx = hist_cjson_str(blk, "text");
            if (!tx || !tx[0]) continue;
            out[emitted++] = (ChatEvent){
                .origin_tool = CHAT_TOOL_COMMANDCODE,
                .role        = text_role,
                .timestamp_ms= t_ms,
                .text        = hist_strdup(a, tx),
                .tool_name   = NULL,
                .session_id  = sid_own,
            };
        } else if (strcmp(bt, "tool-call") == 0) {
            cJSON *input = cJSON_GetObjectItemCaseSensitive(blk, "input");
            out[emitted++] = (ChatEvent){
                .origin_tool = CHAT_TOOL_COMMANDCODE,
                .role        = CHAT_ROLE_TOOL_USE,
                .timestamp_ms= t_ms,
                .text        = hist_cjson_serialize_compact(a, input),
                .tool_name   = hist_strdup(a, hist_cjson_str(blk, "toolName")),
                .session_id  = sid_own,
            };
        } else if (strcmp(bt, "tool-result") == 0) {
            cJSON *output = cJSON_GetObjectItemCaseSensitive(blk, "output");
            char *txt = NULL;
            if (cJSON_IsString(output)) txt = hist_strdup(a, output->valuestring);
            else if (output)            txt = hist_cjson_serialize_compact(a, output);
            out[emitted++] = (ChatEvent){
                .origin_tool = CHAT_TOOL_COMMANDCODE,
                .role        = CHAT_ROLE_TOOL_RESULT,
                .timestamp_ms= t_ms,
                .text        = txt,
                .tool_name   = hist_strdup(a, hist_cjson_str(blk, "toolName")),
                .session_id  = sid_own,
            };
        }
        /* "reasoning" / "image" → skip */
    }

done:
    cJSON_Delete(root);
    return emitted;
}
