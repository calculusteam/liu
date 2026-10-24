/*
 * Bridge-native source parser for Claude Code JSONL session files. Preserve
 * user and assistant messages, tool-call arguments, and call/result pairing;
 * skip metadata, sidechain records, and injected notes that are not
 * conversation content. Keep this parser separate from parser_claude.c because
 * migration needs to retain tool-call details that the display parser folds
 * together.
 */
#include "history/bridge/sources.h"
#include "history/bridge/sources/src_jsonl.h"
#include "history/bridge/tool_map.h"
#include "history/util.h"

#include "cJSON.h"

#include <string.h>

typedef struct {
    bool saw_meta;
} ClaudeCtx;

static bool claude_line(void *root_cjson, Arena *scratch, BridgeHistory *h,
                        void *user) {
    (void)scratch;
    cJSON *root = (cJSON *)root_cjson;
    ClaudeCtx *ctx = user;

    const char *type = hist_cjson_str(root, "type");
    if (!type) return true;

    bool is_user = strcmp(type, "user") == 0;
    bool is_asst = strcmp(type, "assistant") == 0;
    if (!is_user && !is_asst) return true;

    /* A subagent's transcript is a different conversation that happens to
     * share a file. Importing it interleaves two agents' turns. */
    if (hist_cjson_bool(root, "isSidechain")) return true;

    cJSON *msg = cJSON_GetObjectItemCaseSensitive(root, "message");
    if (!msg) return true;
    cJSON *content = cJSON_GetObjectItemCaseSensitive(msg, "content");
    if (!content) return true;

    i64 ts = hist_parse_iso8601_ms(hist_cjson_str(root, "timestamp"));

    /* Session metadata rides on every line; take it from the first one that
     * has it so a session whose header line was truncated still places
     * correctly. */
    if (!ctx->saw_meta) {
        bridge_history_set_session_id(h, hist_cjson_str(root, "sessionId"));
        bridge_history_set_cwd(h, hist_cjson_str(root, "cwd"));
        const char *branch = hist_cjson_str(root, "gitBranch");
        if (branch && branch[0]) bridge_history_set_git(h, NULL, branch, NULL);
        if (is_asst) bridge_history_set_model(h, hist_cjson_str(msg, "model"));
        if (h->cwd) ctx->saw_meta = true;
    } else if (is_asst && !h->source_model) {
        bridge_history_set_model(h, hist_cjson_str(msg, "model"));
    }

    /* isMeta marks text Claude injected on the user's behalf — command
     * caveats, hook output, IDE notices. It is on the user line but is not
     * something the user said, and a tool_result line is never isMeta, so
     * gating the whole line here loses nothing real. */
    if (is_user && hist_cjson_bool(root, "isMeta")) return true;

    BridgeEventType et = is_user ? BRIDGE_EVENT_USER : BRIDGE_EVENT_ASSISTANT;

    /* A slash-command echo is the CLI's record of a keystroke, not a message.
     * Claude writes it as a user line whose whole content is
     *   <command-name>/clear</command-name>
     *   <command-message>clear</command-message>
     *   <command-args></command-args>
     * and — unlike the caveat lines right beside it — does NOT mark it
     * isMeta, so it survives the gate above. Imported as a user turn it shows
     * the next agent an XML fragment, and because it is usually the FIRST
     * user line in a session it would also become the session title.
     * Reclassified as context, which the title fallback skips and every
     * target renders as background. */
    if (is_user && cJSON_IsString(content) && content->valuestring &&
        strncmp(content->valuestring, "<command-name>", 14) == 0) {
        et = BRIDGE_EVENT_CONTEXT;
    }

    bridge_push_anthropic_content(h, CHAT_TOOL_CLAUDE, content, et, ts);
    return true;
}

bool bridge_parse_claude(Arena *a, const ChatSessionMeta *m, BridgeHistory *h) {
    (void)a;
    if (!m || !m->path) return false;
    ClaudeCtx ctx = {0};
    u64 bad = 0;
    bridge_jsonl_run(m->path, h, claude_line, &ctx, &bad);
    /* A truncated JSONL tail is a real loss and the user should be told the
     * migration is partial rather than shown a conversation that just stops. */
    if (bad > 0) {
        h->fidelity.events_dropped += (u32)(bad > 0xFFFFFFFFu ? 0xFFFFFFFFu : bad);
        h->fidelity.history_complete = false;
    }
    return h->event_count > 0;
}
