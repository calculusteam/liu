/*
 * liu-history - Antigravity JSONL line parser.
 *
 * Session files live at ~/.gemini/antigravity/code_tracker/<project>/session*.jsonl
 * (see cli-continues antigravity.ts). Each line is a single JSON record:
 *     { "type": "user"|"assistant"|..., "content": "...", "timestamp": "ISO8601" }
 * We only emit user/assistant turns — tool events and other types are skipped.
 *
 * Some session files carry a short binary prefix before the first "{"; we scan
 * forward to the first brace to salvage them (matches cli-continues behavior).
 */
#include "history/parser.h"
#include "history/event.h"
#include "history/util.h"

#include "cJSON.h"

#include <string.h>

#define cjson_str hist_cjson_str

u32 parser_antigravity_line(Arena *a, const char *line, usize len,
                            ChatEvent *out, u32 out_cap) {
    if (!line || !len || !out || !out_cap) return 0;
    if (len > 4 * 1024 * 1024) return 0;

    /* Skip leading non-'{' bytes — some files have a short binary prefix. */
    const char *p = line;
    usize rem = len;
    while (rem && *p != '{') { p++; rem--; }
    if (!rem) return 0;

    cJSON *root = cJSON_ParseWithLength(p, rem);
    if (!root) return 0;

    u32 emitted = 0;
    const char *type = cjson_str(root, "type");
    const char *content = cjson_str(root, "content");
    const char *ts = cjson_str(root, "timestamp");
    if (!type || !content) goto done;

    ChatRole role;
    if (strcmp(type, "user") == 0)           role = CHAT_ROLE_USER;
    else if (strcmp(type, "assistant") == 0) role = CHAT_ROLE_ASSISTANT;
    else                                     goto done; /* tool/system events skipped */

    if (emitted < out_cap) {
        out[emitted++] = (ChatEvent){
            .origin_tool  = CHAT_TOOL_ANTIGRAVITY,
            .role         = role,
            .timestamp_ms = hist_parse_iso8601_ms(ts),
            .text         = hist_strdup(a, content),
            .tool_name    = NULL,
            .session_id   = NULL,
        };
    }

done:
    cJSON_Delete(root);
    return emitted;
}
