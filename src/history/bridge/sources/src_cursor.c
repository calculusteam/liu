/*
 * liu-history/bridge - Cursor Agent source parser (bridge-native).
 *
 * Store: ~/.cursor/projects/<slugged-cwd>/agent-transcripts/<sess-id>/<sess-id>.jsonl
 *
 * The transcript is Anthropic-shaped:
 *   {"role":"user",     "message":{"content":[{type:"text",text}]}}
 *   {"role":"assistant","message":{"content":[{type:"text"},{type:"tool_use",name,input}]}}
 *   {"type":"turn_ended","status":"error","error":"User aborted request"}
 *
 * One property decides how far a Cursor migration can go: transcripts hold
 * `tool_use` blocks and no `tool_result` blocks — Cursor does not persist
 * tool OUTPUT in its transcript at all. So a Cursor source can carry what the
 * agent DID but never what it saw back, and `tool_history_complete` is false
 * for every migration out of Cursor. That is a property of the store, not a
 * gap in this parser, and it is reported rather than papered over.
 *
 * `tool_use` blocks here also carry no `id`, so ir.c synthesizes the call ids.
 * That is harmless precisely because there are no results to pair them with.
 *
 * The user's real question is wrapped in <user_query>…</user_query> and
 * preceded by harness blocks (<timestamp>, <image_files>, ```browser_element).
 * Unlike Grok's, Cursor's wrapper sits INSIDE a larger text block rather than
 * around the whole of it, so the extraction is a substring search, not a
 * prefix test.
 */
#include "history/bridge/sources.h"
#include "history/bridge/sources/src_jsonl.h"
#include "history/bridge/tool_map.h"
#include "history/util.h"

#include "cJSON.h"

#include <string.h>

/* Pull the <user_query> body out of a mixed text block. Returns the original
 * pointer when there is no wrapper — a user turn without one is still a user
 * turn (short follow-ups arrive bare). */
static const char *extract_query(Arena *a, const char *s) {
    if (!s) return NULL;
    const char *open = strstr(s, "<user_query>");
    if (!open) return s;
    open += 12;
    const char *close = strstr(open, "</user_query>");
    if (!close) return s;
    while (*open == '\n' || *open == ' ' || *open == '\t' || *open == '\r') open++;
    while (close > open && (close[-1] == '\n' || close[-1] == ' ' ||
                            close[-1] == '\t' || close[-1] == '\r')) close--;
    usize n = (usize)(close - open);
    if (n == 0) return NULL;
    char *buf = arena_alloc(a, n + 1);
    if (!buf) return s;
    memcpy(buf, open, n);
    buf[n] = '\0';
    return buf;
}

static bool cursor_line(void *root_cjson, Arena *scratch, BridgeHistory *h,
                        void *user) {
    (void)user;
    cJSON *root = (cJSON *)root_cjson;

    const char *role = hist_cjson_str(root, "role");
    if (!role) return true;   /* turn_ended and friends */

    cJSON *msg = cJSON_GetObjectItemCaseSensitive(root, "message");
    if (!msg) return true;
    cJSON *content = cJSON_GetObjectItemCaseSensitive(msg, "content");
    if (!content) return true;

    bool is_user = strcmp(role, "user") == 0;

    if (is_user) {
        /* Walk the blocks by hand rather than through
         * bridge_push_anthropic_content: only the user channel needs the
         * <user_query> extraction, and doing it inside the shared helper
         * would apply Cursor's framing to every Anthropic-shaped source. */
        if (cJSON_IsString(content)) {
            bridge_push_message(h, BRIDGE_EVENT_USER, 0,
                                extract_query(scratch, content->valuestring));
            return true;
        }
        cJSON *part = NULL;
        cJSON_ArrayForEach(part, content) {
            const char *pt = hist_cjson_str(part, "type");
            if (!pt || strcmp(pt, "text") != 0) continue;
            const char *tx = hist_cjson_str(part, "text");
            bridge_push_message(h, BRIDGE_EVENT_USER, 0, extract_query(scratch, tx));
        }
        return true;
    }

    bridge_push_anthropic_content(h, CHAT_TOOL_CURSOR, content,
                                  BRIDGE_EVENT_ASSISTANT, 0);
    return true;
}

bool bridge_parse_cursor(Arena *a, const ChatSessionMeta *m, BridgeHistory *h) {
    (void)a;
    if (!m || !m->path) return false;

    /* The project directory is Claude's slug convention ('/' -> '-'), which
     * the scanner does NOT decode for Cursor because `empty-window` is also a
     * valid project name there. Decoding is left to the scanner's cwd field;
     * an absolute project string is used when one is present. */
    if (!h->cwd && m->project && m->project[0] == '/')
        bridge_history_set_cwd(h, m->project);

    u64 bad = 0;
    bridge_jsonl_run(m->path, h, cursor_line, NULL, &bad);
    if (bad > 0) {
        h->fidelity.events_dropped += (u32)(bad > 0xFFFFFFFFu ? 0xFFFFFFFFu : bad);
        h->fidelity.history_complete = false;
    }
    return h->event_count > 0;
}
