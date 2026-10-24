/*
 * liu-history/bridge - Grok (xAI CLI) source parser.
 *
 * Store: ~/.grok/sessions/[<percent-encoded-cwd>/]<uuid>/, one directory per
 * session. The scanner registers chat_history.jsonl; summary.json contains
 * the working directory, model, times, and agent name. system_prompt.txt is
 * not imported.
 *
 * chat_history.jsonl contains system, user, assistant, tool_result, and
 * reasoning records. User records with synthetic_reason contain injected
 * context, not a user turn, so they are imported as CONTEXT. User turns
 * wrapped in <user_query> are unwrapped; the first turn may be preceded by a
 * <user_info> block. Reasoning records are skipped because encrypted_content
 * is tied to xAI and the summary is private model output.
 */
#include "history/bridge/sources.h"
#include "history/bridge/sources/src_jsonl.h"
#include "history/bridge/tool_map.h"
#include "history/util.h"

#include "cJSON.h"

#include <stdio.h>
#include <string.h>

/* Strip one <tag>…</tag> wrapper when the whole string is inside it. Returns
 * the interior (a slice of `s`, so no allocation) and shortens `*len`. */
static const char *unwrap(const char *s, usize *len, const char *tag) {
    if (!s || !len) return s;
    usize n = *len;
    char open[32], close[32];
    int on = snprintf(open,  sizeof open,  "<%s>",  tag);
    int cn = snprintf(close, sizeof close, "</%s>", tag);
    if (on <= 0 || cn <= 0) return s;
    usize ol = (usize)on, cl = (usize)cn;

    /* Tolerate surrounding whitespace: the wrapper is usually on its own
     * lines, so an exact-prefix test would miss every real record. */
    usize a = 0, b = n;
    while (a < b && (s[a] == '\n' || s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) a++;
    while (b > a && (s[b-1] == '\n' || s[b-1] == ' ' || s[b-1] == '\t' || s[b-1] == '\r')) b--;
    if (b - a < ol + cl) return s;
    if (strncmp(s + a, open, ol) != 0) return s;
    if (strncmp(s + b - cl, close, cl) != 0) return s;

    a += ol;
    b -= cl;
    while (a < b && (s[a] == '\n' || s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) a++;
    while (b > a && (s[b-1] == '\n' || s[b-1] == ' ' || s[b-1] == '\t' || s[b-1] == '\r')) b--;
    *len = b - a;
    return s + a;
}

/* Grok's content is a bare string or an array of blocks. Only `text` blocks
 * are taken; `reasoning` / `summary_text` blocks are the thinking channel. */
static const char *content_text(Arena *a, cJSON *content) {
    if (!content) return NULL;
    if (cJSON_IsString(content))
        return content->valuestring && content->valuestring[0] ? content->valuestring : NULL;
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
    if (!total) return NULL;
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
                memcpy(buf + off, tx, n);
                off += n;
            }
        }
    }
    buf[off] = '\0';
    return buf;
}

static bool grok_line(void *root_cjson, Arena *scratch, BridgeHistory *h,
                      void *user) {
    (void)user;
    cJSON *root = (cJSON *)root_cjson;
    const char *type = hist_cjson_str(root, "type");
    if (!type) return true;

    /* The harness prompt. Never imported: it names Grok's own tools and
     * safety rules, which are false in every other agent. */
    if (strcmp(type, "system") == 0) return true;
    if (strcmp(type, "reasoning") == 0) return true;

    cJSON *content = cJSON_GetObjectItemCaseSensitive(root, "content");

    if (strcmp(type, "user") == 0) {
        const char *txt = content_text(scratch, content);
        if (!txt) return true;

        bool synthetic = cJSON_GetObjectItemCaseSensitive(root, "synthetic_reason") != NULL;
        usize len = strlen(txt);
        const char *body = unwrap(txt, &len, "user_query");
        bool was_query = (body != txt) || len != strlen(txt);

        /* A user line that is neither a <user_query> nor marked synthetic is
         * still harness-injected: the <user_info> environment block arrives
         * exactly this way. Classify by what it IS, not by the channel. */
        BridgeEventType et = (was_query && !synthetic)
                                ? BRIDGE_EVENT_USER : BRIDGE_EVENT_CONTEXT;

        BridgeEvent e = {0};
        e.type         = et;
        e.timestamp_ms = 0;   /* chat_history lines carry no timestamp */
        char *tmp = arena_alloc(scratch, len + 1);
        if (!tmp) return true;
        memcpy(tmp, body, len);
        tmp[len] = '\0';
        e.text = tmp;
        bridge_history_push(h, &e);
        return true;
    }

    if (strcmp(type, "assistant") == 0) {
        if (!h->source_model) bridge_history_set_model(h, hist_cjson_str(root, "model_id"));
        const char *txt = content_text(scratch, content);
        if (txt) bridge_push_message(h, BRIDGE_EVENT_ASSISTANT, 0, txt);

        /* Tool calls hang off the assistant line in a sibling array rather
         * than inside content, which is why parser_grok.c cannot express
         * them: it only ever looks at `content`. */
        cJSON *calls = cJSON_GetObjectItemCaseSensitive(root, "tool_calls");
        cJSON *call = NULL;
        cJSON_ArrayForEach(call, calls) {
            /* Two spellings in the wild: flat {id,name,arguments} and the
             * OpenAI-shaped {id, function:{name, arguments}}. */
            cJSON *fn = cJSON_GetObjectItemCaseSensitive(call, "function");
            const char *name = hist_cjson_str(fn ? fn : call, "name");
            cJSON *args = cJSON_GetObjectItemCaseSensitive(fn ? fn : call, "arguments");
            BridgeEvent e = {0};
            e.type         = BRIDGE_EVENT_TOOL_CALL;
            e.tool_name    = name;
            e.tool_call_id = hist_cjson_str(call, "id");
            e.tool_kind    = bridge_tool_kind_for(CHAT_TOOL_XAI, name);
            e.tool_input   = bridge_json_compact(scratch, args);
            bridge_history_push(h, &e);
        }
        return true;
    }

    if (strcmp(type, "tool_result") == 0 || strcmp(type, "tool") == 0) {
        const char *out = content_text(scratch, content);
        if (!out && content) out = hist_cjson_serialize_compact(scratch, content);
        BridgeEvent e = {0};
        e.type         = BRIDGE_EVENT_TOOL_RESULT;
        e.tool_call_id = hist_cjson_str(root, "tool_call_id");
        e.tool_name    = hist_cjson_str(root, "name");
        e.tool_kind    = bridge_tool_kind_for(CHAT_TOOL_XAI, e.tool_name);
        e.tool_output  = out;
        bridge_history_push(h, &e);
        return true;
    }
    return true;
}

/* summary.json contains the session working directory and model. The scanner
 * reads the directory; this parser also reads the file so callers with only
 * the chat path do not depend on ChatSessionMeta. */
static void grok_read_summary(BridgeHistory *h, const char *chat_path) {
    if (!chat_path) return;
    const char *slash = strrchr(chat_path, '/');
    if (!slash) return;
    usize dirlen = (usize)(slash - chat_path);
    char sum[CHAT_PATH_CAP];
    if (dirlen + 16 >= sizeof sum) return;
    memcpy(sum, chat_path, dirlen);
    snprintf(sum + dirlen, sizeof sum - dirlen, "/summary.json");

    cJSON *root = (cJSON *)bridge_json_slurp(sum, KB(256));
    if (!root) return;
    cJSON *info = cJSON_GetObjectItemCaseSensitive(root, "info");
    if (info) {
        if (!h->cwd) bridge_history_set_cwd(h, hist_cjson_str(info, "cwd"));
        if (!h->source_session_id)
            bridge_history_set_session_id(h, hist_cjson_str(info, "id"));
    }
    bridge_history_set_model(h, hist_cjson_str(root, "current_model_id"));
    const char *summary = hist_cjson_str(root, "session_summary");
    if (summary && summary[0]) bridge_history_set_title(h, summary);
    cJSON_Delete(root);
}

bool bridge_parse_grok(Arena *a, const ChatSessionMeta *m, BridgeHistory *h) {
    (void)a;
    if (!m || !m->path) return false;
    grok_read_summary(h, m->path);
    u64 bad = 0;
    bridge_jsonl_run(m->path, h, grok_line, NULL, &bad);
    if (bad > 0) {
        h->fidelity.events_dropped += (u32)(bad > 0xFFFFFFFFu ? 0xFFFFFFFFu : bad);
        h->fidelity.history_complete = false;
    }
    return h->event_count > 0;
}
