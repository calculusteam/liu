/*
 * liu-history - Cline-family ui_messages.json parser.
 *
 * Cline, Roo Code, and Kilo Code are all editor extensions that share a
 * single on-disk format: a JSON array in <extension>/tasks/<task-id>/
 * ui_messages.json. Each entry:
 *     { "ts": <ms>, "type": "say"|"ask", "say"?: "...", "text"?: "...",
 *       "partial"?: bool, ... }
 *
 * Role classification (cli-continues cline.ts classifyRole + buildConversation):
 *   type != "say"                              → skipped
 *   say == "user_feedback"                     → user
 *   say == "text" && !partial                  → user
 *   say == "text" &&  partial                  → assistant (streaming chunk)
 *   say == "completion_result"                 → assistant
 *   say == "reasoning"                         → assistant
 *   other say values (api_req_*, etc.)         → skipped
 *
 * Consecutive partial assistant streaming chunks are deduplicated — only the
 * last (most complete) one is kept.
 *
 * Unlike the JSONL parsers this one reads the whole file in one shot, so it
 * exposes a `parser_cline_run` entry point that drives the callback directly.
 */
#include "history/parser.h"
#include "history/event.h"
#include "history/util.h"

#include "cJSON.h"

#include <string.h>

#define cjson_str  hist_cjson_str
#define cjson_bool hist_cjson_bool

/* Returns CHAT_ROLE_SYSTEM as a "skip" sentinel so callers can branch on it. */
static ChatRole classify(const char *type, const char *say, bool partial) {
    if (!type || strcmp(type, "say") != 0) return CHAT_ROLE_SYSTEM;
    if (!say) return CHAT_ROLE_SYSTEM;
    if (strcmp(say, "user_feedback") == 0)      return CHAT_ROLE_USER;
    if (strcmp(say, "completion_result") == 0)  return CHAT_ROLE_ASSISTANT;
    if (strcmp(say, "reasoning") == 0)          return CHAT_ROLE_ASSISTANT;
    if (strcmp(say, "text") == 0)               return partial ? CHAT_ROLE_ASSISTANT : CHAT_ROLE_USER;
    return CHAT_ROLE_SYSTEM;
}

/* Copy a string with leading/trailing ASCII whitespace stripped. Returns NULL
 * for empty / all-whitespace input. */
static char *arena_trim_dup(Arena *a, const char *s) {
    if (!s) return NULL;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    usize n = strlen(s);
    while (n > 0) {
        char c = s[n - 1];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
        n--;
    }
    if (n == 0) return NULL;
    return hist_strndup(a, s, n);
}

u64 parser_cline_run(Arena *a, ChatTool tool, const char *path,
                     chat_event_cb cb, void *user) {
    if (!a || !path || !cb) return 0;

    /* Hard cap at 64 MiB — ui_messages.json in the wild can be large but this
     * is big enough for a full project's history with a margin. */
    char *buf = NULL;
    usize off = 0;
    if (!hist_slurp_file(a, path, 64 * 1024 * 1024, &buf, &off)) return 0;

    cJSON *root = cJSON_ParseWithLength(buf, off);
    if (!root || !cJSON_IsArray(root)) { cJSON_Delete(root); return 0; }

    u64 emitted = 0;
    bool prev_assistant_partial = false;
    ChatEvent pending = {0};
    bool has_pending = false;

    cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, root) {
        const char *type = cjson_str(entry, "type");
        const char *say  = cjson_str(entry, "say");
        const char *text = cjson_str(entry, "text");
        bool partial = cjson_bool(entry, "partial");

        ChatRole role = classify(type, say, partial);
        if (role == CHAT_ROLE_SYSTEM) continue;  /* skip */
        if (!text) continue;

        /* No arena_reset here — the previous pending event's strings must
         * remain valid until we flush it via the callback. The caller (see
         * app_open_transcript_viewer) sizes the arena for the whole file. */
        char *trimmed = arena_trim_dup(a, text);
        if (!trimmed) continue;

        /* Dedupe consecutive partial assistant "text" chunks: replace in place. */
        bool is_assistant_partial = (role == CHAT_ROLE_ASSISTANT &&
                                     say && strcmp(say, "text") == 0 && partial);
        i64 ts_ms = 0;
        cJSON *ts = cJSON_GetObjectItemCaseSensitive(entry, "ts");
        if (ts && cJSON_IsNumber(ts)) ts_ms = (i64)ts->valuedouble;

        ChatEvent ev = {
            .origin_tool  = (ChatTool)tool,
            .role         = role,
            .timestamp_ms = ts_ms,
            .text         = trimmed,
            .tool_name    = NULL,
            .session_id   = NULL,
        };

        if (has_pending && is_assistant_partial && prev_assistant_partial) {
            /* Replace previous partial chunk with this longer one. */
            pending = ev;
        } else {
            /* Flush previous pending event. */
            if (has_pending) {
                emitted++;
                if (!cb(&pending, user)) goto done;
            }
            pending = ev;
            has_pending = true;
        }
        prev_assistant_partial = is_assistant_partial;
    }
    if (has_pending) {
        emitted++;
        cb(&pending, user);
    }

done:
    cJSON_Delete(root);
    return emitted;
}
