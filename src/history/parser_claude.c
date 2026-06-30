/*
 * liu-history - Claude Code JSONL line parser.
 *
 * Records we care about (`type` field in each line):
 *   user      + message.content is string  → CHAT_ROLE_USER
 *   user      + message.content is list    → iterate items; tool_result → TOOL_RESULT
 *   assistant + message.content is list    → iterate items; text → ASSISTANT,
 *                                              tool_use → TOOL_USE
 *
 * Record types we skip silently: file-history-snapshot, permission-mode,
 * system, and anything else.
 */
#include "history/parser.h"
#include "history/event.h"
#include "history/util.h"

#include "cJSON.h"

#include <string.h>

/* Join an assistant content-array's textual parts with newlines. */
static char *join_text_parts(Arena *a, cJSON *parts) {
    if (!parts || !cJSON_IsArray(parts)) return NULL;
    usize total = 0;
    cJSON *it = NULL;
    cJSON_ArrayForEach(it, parts) {
        const char *tp = hist_cjson_str(it, "type");
        if (tp && strcmp(tp, "text") == 0) {
            const char *tx = hist_cjson_str(it, "text");
            if (tx) total += strlen(tx) + 1;
        } else if (tp && strcmp(tp, "tool_result") == 0) {
            /* nested content — serialize */
            cJSON *c = cJSON_GetObjectItemCaseSensitive(it, "content");
            if (c) {
                char *s = cJSON_PrintUnformatted(c);
                if (s) { total += strlen(s) + 1; cJSON_free(s); }
            }
        }
    }
    if (total == 0) return NULL;
    char *buf = arena_alloc(a, total + 1);
    if (!buf) return NULL;
    usize off = 0;
    cJSON_ArrayForEach(it, parts) {
        const char *tp = hist_cjson_str(it, "type");
        if (tp && strcmp(tp, "text") == 0) {
            const char *tx = hist_cjson_str(it, "text");
            if (tx) {
                usize n = strlen(tx);
                if (off > 0 && off < total) buf[off++] = '\n';
                memcpy(buf + off, tx, n); off += n;
            }
        } else if (tp && strcmp(tp, "tool_result") == 0) {
            cJSON *c = cJSON_GetObjectItemCaseSensitive(it, "content");
            if (c) {
                char *s = cJSON_PrintUnformatted(c);
                if (s) {
                    usize n = strlen(s);
                    if (off > 0 && off < total) buf[off++] = '\n';
                    if (off + n > total) n = total - off;
                    memcpy(buf + off, s, n); off += n;
                    cJSON_free(s);
                }
            }
        }
    }
    buf[off] = '\0';
    return buf;
}

u32 parser_claude_line(Arena *a, const char *line, usize len,
                       ChatEvent *out, u32 out_cap) {
    if (!line || !len || !out || !out_cap) return 0;
    if (len > 4 * 1024 * 1024) return 0;  /* paranoia cap */

    cJSON *root = cJSON_ParseWithLength(line, len);
    if (!root) return 0;

    u32 emitted = 0;

    const char *type  = hist_cjson_str(root, "type");
    const char *ts    = hist_cjson_str(root, "timestamp");
    const char *sid   = hist_cjson_str(root, "sessionId");
    if (!type) goto done;

    i64 t_ms = hist_parse_iso8601_ms(ts);
    char *sid_own = hist_strdup(a, sid);

    if (strcmp(type, "user") == 0) {
        cJSON *msg = cJSON_GetObjectItemCaseSensitive(root, "message");
        if (!msg) goto done;
        cJSON *content = cJSON_GetObjectItemCaseSensitive(msg, "content");
        if (!content) goto done;

        if (cJSON_IsString(content)) {
            if (emitted < out_cap) {
                out[emitted++] = (ChatEvent){
                    .origin_tool = CHAT_TOOL_CLAUDE,
                    .role        = CHAT_ROLE_USER,
                    .timestamp_ms= t_ms,
                    .text        = hist_strdup(a, content->valuestring),
                    .tool_name   = NULL,
                    .session_id  = sid_own,
                };
            }
        } else if (cJSON_IsArray(content)) {
            /* Usually a list of tool_result blocks. */
            cJSON *part = NULL;
            cJSON_ArrayForEach(part, content) {
                const char *ptype = hist_cjson_str(part, "type");
                if (!ptype) continue;
                if (strcmp(ptype, "tool_result") == 0 && emitted < out_cap) {
                    cJSON *inner = cJSON_GetObjectItemCaseSensitive(part, "content");
                    char *txt = NULL;
                    if (cJSON_IsString(inner)) {
                        txt = hist_strdup(a, inner->valuestring);
                    } else if (cJSON_IsArray(inner)) {
                        txt = join_text_parts(a, inner);
                        if (!txt) txt = hist_cjson_serialize_compact(a, inner);
                    } else if (inner) {
                        txt = hist_cjson_serialize_compact(a, inner);
                    }
                    const char *tool_ref = hist_cjson_str(part, "tool_use_id");
                    out[emitted++] = (ChatEvent){
                        .origin_tool = CHAT_TOOL_CLAUDE,
                        .role        = CHAT_ROLE_TOOL_RESULT,
                        .timestamp_ms= t_ms,
                        .text        = txt,
                        .tool_name   = hist_strdup(a, tool_ref),
                        .session_id  = sid_own,
                    };
                } else if (strcmp(ptype, "text") == 0 && emitted < out_cap) {
                    out[emitted++] = (ChatEvent){
                        .origin_tool = CHAT_TOOL_CLAUDE,
                        .role        = CHAT_ROLE_USER,
                        .timestamp_ms= t_ms,
                        .text        = hist_strdup(a, hist_cjson_str(part, "text")),
                        .tool_name   = NULL,
                        .session_id  = sid_own,
                    };
                }
            }
        }
    } else if (strcmp(type, "assistant") == 0) {
        cJSON *msg = cJSON_GetObjectItemCaseSensitive(root, "message");
        if (!msg) goto done;
        cJSON *content = cJSON_GetObjectItemCaseSensitive(msg, "content");
        if (!content || !cJSON_IsArray(content)) goto done;

        cJSON *part = NULL;
        cJSON_ArrayForEach(part, content) {
            if (emitted >= out_cap) break;
            const char *ptype = hist_cjson_str(part, "type");
            if (!ptype) continue;
            if (strcmp(ptype, "text") == 0) {
                out[emitted++] = (ChatEvent){
                    .origin_tool = CHAT_TOOL_CLAUDE,
                    .role        = CHAT_ROLE_ASSISTANT,
                    .timestamp_ms= t_ms,
                    .text        = hist_strdup(a, hist_cjson_str(part, "text")),
                    .tool_name   = NULL,
                    .session_id  = sid_own,
                };
            } else if (strcmp(ptype, "tool_use") == 0) {
                cJSON *input = cJSON_GetObjectItemCaseSensitive(part, "input");
                out[emitted++] = (ChatEvent){
                    .origin_tool = CHAT_TOOL_CLAUDE,
                    .role        = CHAT_ROLE_TOOL_USE,
                    .timestamp_ms= t_ms,
                    .text        = hist_cjson_serialize_compact(a, input),
                    .tool_name   = hist_strdup(a, hist_cjson_str(part, "name")),
                    .session_id  = sid_own,
                };
            }
        }
    }
    /* other types (file-history-snapshot, permission-mode) → silently skip */

done:
    cJSON_Delete(root);
    return emitted;
}
