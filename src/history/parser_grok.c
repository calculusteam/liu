/*
 * liu-history - Grok (xAI `grok` CLI) chat_history.jsonl line parser.
 *
 * Session store: ~/.grok/sessions/<session-uuid>/chat_history.jsonl, with
 * project-scoped sessions nested one level deeper under a percent-encoded
 * cwd directory (~/.grok/sessions/%2FUsers%2F…/<uuid>/…). One JSON object
 * per line:
 *   {"type":"system","content":"…"}                            → skipped
 *   {"type":"user","content":"…"|[{"type":"text","text":"…"}]} → USER
 *   {"type":"assistant","content":"…"|[blocks],
 *    "tool_calls":[{"id","name","arguments"}…]}                → ASSISTANT
 *                                                                (+ TOOL_USE per call)
 *   {"type":"tool_result","tool_call_id":"…","content":…}      → TOOL_RESULT
 *
 * Assistant content arrays also carry "reasoning" / "summary_text" thinking
 * blocks — only "text" blocks are surfaced. Lines carry no timestamps (the
 * session's summary.json owns created/updated times), so timestamp_ms is 0
 * and the picker falls back to file mtime from the scanner.
 */
#include "history/parser.h"
#include "history/event.h"
#include "history/util.h"

#include "cJSON.h"

#include <string.h>

/* content → arena string. Accepts a plain string or an array of blocks,
 * joining only the "text" blocks with newlines (reasoning/summary_text are
 * thinking-channel noise for transcript purposes). */
static char *grok_content_text(Arena *a, cJSON *content) {
    if (!content) return NULL;
    if (cJSON_IsString(content)) {
        if (!content->valuestring || !content->valuestring[0]) return NULL;
        return hist_strdup(a, content->valuestring);
    }
    if (!cJSON_IsArray(content)) return NULL;

    usize total = 0;
    cJSON *it = NULL;
    cJSON_ArrayForEach(it, content) {
        const char *tp = hist_cjson_str(it, "type");
        if (tp && strcmp(tp, "text") == 0) {
            const char *tx = hist_cjson_str(it, "text");
            if (tx) total += strlen(tx) + 1;
        }
    }
    if (total == 0) return NULL;
    char *buf = arena_alloc(a, total + 1);
    if (!buf) return NULL;
    usize off = 0;
    cJSON_ArrayForEach(it, content) {
        const char *tp = hist_cjson_str(it, "type");
        if (tp && strcmp(tp, "text") == 0) {
            const char *tx = hist_cjson_str(it, "text");
            if (tx) {
                usize n = strlen(tx);
                if (off > 0 && off < total) buf[off++] = '\n';
                memcpy(buf + off, tx, n); off += n;
            }
        }
    }
    buf[off] = '\0';
    return buf;
}

u32 parser_grok_line(Arena *a, const char *line, usize len,
                     ChatEvent *out, u32 out_cap) {
    if (!line || !len || !out || !out_cap) return 0;
    if (len > 4 * 1024 * 1024) return 0;  /* paranoia cap */

    cJSON *root = cJSON_ParseWithLength(line, len);
    if (!root) return 0;

    u32 emitted = 0;
    const char *type = hist_cjson_str(root, "type");
    if (!type) goto done;

    if (strcmp(type, "user") == 0) {
        cJSON *content = cJSON_GetObjectItemCaseSensitive(root, "content");
        char *txt = grok_content_text(a, content);
        if (txt && emitted < out_cap) {
            out[emitted++] = (ChatEvent){
                .origin_tool = CHAT_TOOL_XAI,
                .role        = CHAT_ROLE_USER,
                .timestamp_ms= 0,
                .text        = txt,
                .tool_name   = NULL,
                .session_id  = NULL,
            };
        }
    } else if (strcmp(type, "assistant") == 0) {
        cJSON *content = cJSON_GetObjectItemCaseSensitive(root, "content");
        char *txt = grok_content_text(a, content);
        if (txt && emitted < out_cap) {
            out[emitted++] = (ChatEvent){
                .origin_tool = CHAT_TOOL_XAI,
                .role        = CHAT_ROLE_ASSISTANT,
                .timestamp_ms= 0,
                .text        = txt,
                .tool_name   = NULL,
                .session_id  = NULL,
            };
        }
        cJSON *calls = cJSON_GetObjectItemCaseSensitive(root, "tool_calls");
        if (cJSON_IsArray(calls)) {
            cJSON *call = NULL;
            cJSON_ArrayForEach(call, calls) {
                if (emitted >= out_cap) break;
                const char *name = hist_cjson_str(call, "name");
                const char *args = hist_cjson_str(call, "arguments");
                out[emitted++] = (ChatEvent){
                    .origin_tool = CHAT_TOOL_XAI,
                    .role        = CHAT_ROLE_TOOL_USE,
                    .timestamp_ms= 0,
                    .text        = hist_strdup(a, args),
                    .tool_name   = hist_strdup(a, name),
                    .session_id  = NULL,
                };
            }
        }
    } else if (strcmp(type, "tool_result") == 0) {
        cJSON *content = cJSON_GetObjectItemCaseSensitive(root, "content");
        char *txt = grok_content_text(a, content);
        if (!txt && content) txt = hist_cjson_serialize_compact(a, content);
        if (emitted < out_cap) {
            out[emitted++] = (ChatEvent){
                .origin_tool = CHAT_TOOL_XAI,
                .role        = CHAT_ROLE_TOOL_RESULT,
                .timestamp_ms= 0,
                .text        = txt,
                .tool_name   = hist_strdup(a, hist_cjson_str(root, "tool_call_id")),
                .session_id  = NULL,
            };
        }
    }
    /* "system" and anything else → silently skip */

done:
    cJSON_Delete(root);
    return emitted;
}
