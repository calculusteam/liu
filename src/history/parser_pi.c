/*
 * liu-history - Pi (pi-coding-agent) and Oh My Pi (omp) session .jsonl line parser.
 *
 * Both write the same tree-shaped JSONL (the session format Pi documents; omp
 * is a fork and adds records of its own):
 *
 *   ~/.pi/agent/sessions/--<cwd with / as ->--/<ts>_<uuid>.jsonl
 *   ~/.omp/agent/sessions/--<cwd with / as ->--/<ts>_<uuid>.jsonl
 *
 * One JSON object a line. The first is the header
 *   {"type":"session","version":3,"id":"<uuid>","timestamp":"ISO","cwd":"/abs"}
 * (omp puts a {"type":"title",…} line before it), then entries linked by
 * id/parentId:
 *   {"type":"message","id","parentId","timestamp":"ISO","message":{…}}
 * where message.role is
 *   user          content: string | [{"type":"text","text"}|{"type":"image"}]
 *   assistant     content: [{"type":"text"}|{"type":"thinking"}|
 *                           {"type":"toolCall","id","name","arguments":{…}}]
 *   toolResult    toolCallId, toolName, isError, content: [{"type":"text"}|image]
 *   bashExecution command, output, exitCode          (Pi's `!cmd` in the editor)
 *   custom / developer / branchSummary / compactionSummary — an extension's or
 *                 the harness's own text, never something the user typed.
 * and the other entry types that carry text of their own are
 *   compaction    {"summary":"…"}            branch_summary  {"summary":"…"}
 * (model_change, thinking_level_change, label, custom, custom_message,
 * mode_change, title_change … carry none and are dropped).
 *
 * The thinking channel and images are skipped, as the other parsers do.
 */
#include "history/parser.h"
#include "history/event.h"
#include "history/util.h"

#include "cJSON.h"

#include <string.h>

typedef struct {
    Arena     *a;
    ChatTool   tool;
    i64        t_ms;
    ChatEvent *out;
    u32        cap;
    u32        n;
} PiCtx;

static void pi_push(PiCtx *c, ChatRole role, const char *text, const char *tool_name) {
    if (c->n >= c->cap) return;
    c->out[c->n++] = (ChatEvent){
        .origin_tool  = c->tool,
        .role         = role,
        .timestamp_ms = c->t_ms,
        .text         = text ? hist_strdup(c->a, text) : NULL,
        .tool_name    = tool_name ? hist_strdup(c->a, tool_name) : NULL,
        .session_id   = NULL,
    };
}

/* The text blocks of a content value: one event per non-empty block. */
static void pi_text_blocks(PiCtx *c, cJSON *content, ChatRole role, const char *tool_name) {
    if (cJSON_IsString(content)) {
        if (content->valuestring && content->valuestring[0]) pi_push(c, role, content->valuestring, tool_name);
        return;
    }
    if (!cJSON_IsArray(content)) return;
    cJSON *blk = NULL;
    cJSON_ArrayForEach(blk, content) {
        const char *bt = hist_cjson_str(blk, "type");
        if (!bt || strcmp(bt, "text") != 0) continue;
        const char *tx = hist_cjson_str(blk, "text");
        if (tx && tx[0]) pi_push(c, role, tx, tool_name);
    }
}

u32 parser_pi_line(Arena *a, ChatTool tool, const char *line, usize len,
                   ChatEvent *out, u32 out_cap) {
    if (!line || !len || !out || !out_cap) return 0;
    if (len > 4 * 1024 * 1024) return 0;   /* paranoia cap */

    cJSON *root = cJSON_ParseWithLength(line, len);
    if (!root) return 0;

    PiCtx c = { .a = a, .tool = tool, .t_ms = hist_parse_iso8601_ms(hist_cjson_str(root, "timestamp")),
                .out = out, .cap = out_cap, .n = 0 };
    const char *type = hist_cjson_str(root, "type");
    if (!type) goto done;

    if (strcmp(type, "message") == 0) {
        cJSON *msg = cJSON_GetObjectItemCaseSensitive(root, "message");
        const char *role = msg ? hist_cjson_str(msg, "role") : NULL;
        if (!role) goto done;
        cJSON *content = cJSON_GetObjectItemCaseSensitive(msg, "content");

        if (strcmp(role, "user") == 0) {
            pi_text_blocks(&c, content, CHAT_ROLE_USER, NULL);
        } else if (strcmp(role, "assistant") == 0) {
            if (cJSON_IsString(content)) {
                pi_text_blocks(&c, content, CHAT_ROLE_ASSISTANT, NULL);
            } else if (cJSON_IsArray(content)) {
                cJSON *blk = NULL;
                cJSON_ArrayForEach(blk, content) {
                    if (c.n >= c.cap) break;
                    const char *bt = hist_cjson_str(blk, "type");
                    if (!bt) continue;
                    if (strcmp(bt, "text") == 0) {
                        const char *tx = hist_cjson_str(blk, "text");
                        if (tx && tx[0]) pi_push(&c, CHAT_ROLE_ASSISTANT, tx, NULL);
                    } else if (strcmp(bt, "toolCall") == 0) {
                        cJSON *args = cJSON_GetObjectItemCaseSensitive(blk, "arguments");
                        char *json = args ? hist_cjson_serialize_compact(a, args) : NULL;
                        pi_push(&c, CHAT_ROLE_TOOL_USE, json, hist_cjson_str(blk, "name"));
                    }
                    /* "thinking" → skip */
                }
            }
        } else if (strcmp(role, "toolResult") == 0) {
            pi_text_blocks(&c, content, CHAT_ROLE_TOOL_RESULT, hist_cjson_str(msg, "toolName"));
        } else if (strcmp(role, "bashExecution") == 0) {
            const char *cmd = hist_cjson_str(msg, "command");
            const char *output = hist_cjson_str(msg, "output");
            if (cmd && cmd[0]) pi_push(&c, CHAT_ROLE_TOOL_USE, cmd, "bash");
            if (output && output[0]) pi_push(&c, CHAT_ROLE_TOOL_RESULT, output, "bash");
        }
        /* custom / developer / branchSummary / compactionSummary → skip */
    } else if (strcmp(type, "compaction") == 0 || strcmp(type, "branch_summary") == 0) {
        const char *sum = hist_cjson_str(root, "summary");
        if (sum && sum[0]) pi_push(&c, CHAT_ROLE_SYSTEM, sum, NULL);
    }

done:
    cJSON_Delete(root);
    return c.n;
}
