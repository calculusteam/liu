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

#include <stdlib.h>
#include <string.h>

typedef struct {
    bool saw_meta;
} ClaudeCtx;

/* Bounded text builder for claude_file_change. */
typedef struct { char *p; usize len, cap; bool over; } PatchText;

static void patch_put(PatchText *t, const char *s, usize n) {
    if (t->over) return;
    if (t->len + n + 1 > BRIDGE_TOOL_OUTPUT_MAX) { t->over = true; return; }
    if (t->len + n + 1 > t->cap) {
        usize ncap = t->cap ? t->cap * 2 : 1024;
        while (ncap < t->len + n + 1) ncap *= 2;
        char *q = realloc(t->p, ncap);
        if (!q) { t->over = true; return; }
        t->p = q;
        t->cap = ncap;
    }
    memcpy(t->p + t->len, s, n);
    t->len += n;
    t->p[t->len] = '\0';
}

/* Claude Code records what an Edit, MultiEdit or Write did beside the result
 * line: `toolUseResult.structuredPatch`, hunks numbered in the file as it
 * was, and `type: "create"` for a file the call made. Turned into unified-diff
 * text for the result's event (BridgeEvent.tool_patch); nothing when the line
 * holds no such record or the text would pass the size policy. */
static void claude_file_change(BridgeHistory *h, cJSON *root, usize first) {
    cJSON *tur = cJSON_GetObjectItemCaseSensitive(root, "toolUseResult");
    if (!cJSON_IsObject(tur)) return;
    usize result = h->event_count;
    for (usize i = first; i < h->event_count; i++) {
        if (h->events[i].type != BRIDGE_EVENT_TOOL_RESULT) continue;
        if (result != h->event_count) return;   /* two results: whose patch? */
        result = i;
    }
    if (result == h->event_count) return;

    const char *kind = hist_cjson_str(tur, "type");
    if (kind && strcmp(kind, "create") == 0) {
        bridge_history_set_file_change(h, result, NULL, true);
        return;
    }
    cJSON *hunks = cJSON_GetObjectItemCaseSensitive(tur, "structuredPatch");
    if (!cJSON_IsArray(hunks) || cJSON_GetArraySize(hunks) == 0) return;

    PatchText t = {0};
    cJSON *hk = NULL;
    cJSON_ArrayForEach(hk, hunks) {
        i64 os = 0, ol = 0, ns = 0, nl = 0;
        cJSON *lines = cJSON_GetObjectItemCaseSensitive(hk, "lines");
        if (!hist_cjson_i64(hk, "oldStart", &os) || !hist_cjson_i64(hk, "oldLines", &ol) ||
            !hist_cjson_i64(hk, "newStart", &ns) || !hist_cjson_i64(hk, "newLines", &nl) ||
            !cJSON_IsArray(lines) || os < 0 || ol < 0 || ns < 0 || nl < 0) {
            t.over = true;
            break;
        }
        char head[96];
        int hn = snprintf(head, sizeof head, "@@ -%lld,%lld +%lld,%lld @@\n",
                          (long long)os, (long long)ol, (long long)ns, (long long)nl);
        patch_put(&t, head, (usize)hn);
        cJSON *ln = NULL;
        cJSON_ArrayForEach(ln, lines) {
            if (!cJSON_IsString(ln) || !ln->valuestring) continue;
            const char *l = ln->valuestring;
            /* A line whose marker went missing is context, as diff reads it. */
            if (!(l[0] == ' ' || l[0] == '-' || l[0] == '+' || l[0] == '\\'))
                patch_put(&t, " ", 1);
            patch_put(&t, l, strlen(l));
            patch_put(&t, "\n", 1);
        }
    }
    if (!t.over && t.p) bridge_history_set_file_change(h, result, t.p, false);
    free(t.p);
}

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

    usize first = h->event_count;
    bridge_push_anthropic_content(h, CHAT_TOOL_CLAUDE, content, et, ts);
    if (is_user) claude_file_change(h, root, first);
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
