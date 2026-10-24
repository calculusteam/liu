/*
 * liu-history - Cline CLI session parser.
 *
 * The standalone `cline` binary stores each session as a directory under
 * ~/.cline/data/sessions/<id>/ holding <id>.json (metadata) and
 * <id>.messages.json (the transcript). This parses the transcript.
 *
 * Nothing here is shared with parser_cline.c: that one reads the VS Code
 * extension's ui_messages.json, a flat array of {ts, type:"say"|"ask", text}
 * UI records. The CLI writes Anthropic-shaped messages instead:
 *
 *     { "version": 1, "sessionId": "...", "system_prompt": "...",
 *       "messages": [
 *         { "id": "...", "role": "user"|"assistant", "ts": "<ms>",
 *           "content": "..."                      // plain string, or
 *                    | [ { "type": "text",        "text": "..." },
 *                        { "type": "thinking",    "thinking": "..." },
 *                        { "type": "tool_use",    "name": "...", "input": {} },
 *                        { "type": "tool_result", "name": "...", "content": … } ] } ] }
 *
 * Two shapes worth calling out, both seen in real sessions:
 *
 *   - `ts` is a STRING of milliseconds, not a number. hist_cjson_i64 only
 *     reads JSON numbers, so a numeric read alone returns 0 for every message
 *     and the whole transcript collapses onto the epoch.
 *
 *   - The CLI wraps what the user typed in `<user_input mode="act">…
 *     </user_input>`. That is protocol framing, not something the user wrote,
 *     so it is stripped — otherwise every user turn in the viewer opens with
 *     an XML tag.
 */
#include "history/parser.h"
#include "history/event.h"
#include "history/util.h"

#include "cJSON.h"

#include <stdlib.h>
#include <string.h>

#define cjson_str hist_cjson_str

/* Copy [s, s+n) with leading/trailing ASCII whitespace stripped; NULL when the
 * trimmed span is empty.
 *
 * Length-taking on purpose: the caller's text is a slice of a cJSON string
 * (unwrap_user_input hands back an interior pointer plus a shortened length),
 * so the trim bounds are computed against the borrowed pointer and the arena
 * receives exactly one copy. Materializing the untrimmed slice first would put
 * every message of every transcript into the arena twice, against a fixed 4
 * MiB budget that is never reset. */
static char *trim_dup_n(Arena *a, const char *s, usize n) {
    if (!s) return NULL;
    usize start = 0;
    while (start < n) {
        char c = s[start];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
        start++;
    }
    while (n > start) {
        char c = s[n - 1];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
        n--;
    }
    if (n == start) return NULL;
    return hist_strndup(a, s + start, n - start);
}

/* Strip one `<user_input …>` … `</user_input>` wrapper, if the text is one.
 * Returns a pointer into `s` and adjusts *len; leaves both alone otherwise. */
static const char *unwrap_user_input(const char *s, usize *len) {
    static const char open_tag[] = "<user_input";
    static const char close_tag[] = "</user_input>";
    if (!s || !len || *len < sizeof open_tag) return s;
    if (strncmp(s, open_tag, sizeof open_tag - 1) != 0) return s;
    const char *gt = memchr(s, '>', *len);
    if (!gt) return s;
    const char *body = gt + 1;
    usize body_len = *len - (usize)(body - s);
    if (body_len >= sizeof close_tag - 1 &&
        memcmp(body + body_len - (sizeof close_tag - 1), close_tag,
               sizeof close_tag - 1) == 0) {
        body_len -= sizeof close_tag - 1;
    }
    *len = body_len;
    return body;
}

/* Milliseconds from a field the CLI writes as a decimal string (and older
 * builds may write as a number). 0 when absent or unparseable. */
static i64 read_ts_ms(cJSON *obj) {
    i64 v = 0;
    if (hist_cjson_i64(obj, "ts", &v) && v > 0) return v;
    const char *s = cjson_str(obj, "ts");
    if (!s || !*s) return 0;
    char *end = NULL;
    long long parsed = strtoll(s, &end, 10);
    if (end == s || parsed <= 0) return 0;
    return (i64)parsed;
}

/* Emit one event; returns false when the callback asks to stop. */
static bool emit(Arena *a, chat_event_cb cb, void *user, u64 *emitted,
                 ChatRole role, i64 ts_ms, const char *text, usize text_len,
                 const char *tool_name, const char *session_id) {
    if (!text || text_len == 0) return true;
    /* One arena copy: trim_dup_n reads the caller's borrowed span directly.
     * `text` outlives this call in every case — it points into the cJSON tree
     * (freed at the end of parser_cline_cli_run) or into a cJSON_Print buffer
     * the caller frees only after emit() returns. */
    char *trimmed = trim_dup_n(a, text, text_len);
    if (!trimmed) return true;
    ChatEvent ev = {
        .origin_tool  = CHAT_TOOL_CLINE_CLI,
        .role         = role,
        .timestamp_ms = ts_ms,
        .text         = trimmed,
        .tool_name    = tool_name,
        .session_id   = session_id,
    };
    (*emitted)++;
    return cb(&ev, user);
}

u64 parser_cline_cli_run(Arena *a, const char *path,
                         chat_event_cb cb, void *user) {
    if (!a || !path || !cb) return 0;

    char *buf = NULL;
    usize off = 0;
    if (!hist_slurp_file_heap(path, 64 * 1024 * 1024, &buf, &off)) return 0;

    cJSON *root = cJSON_ParseWithLength(buf, off);
    /* Source bytes die with the parse — see hist_slurp_file_heap. Everything
     * below reads the cJSON tree, never `buf`. */
    free(buf);
    buf = NULL;
    if (!root) return 0;
    cJSON *msgs = cJSON_GetObjectItemCaseSensitive(root, "messages");
    if (!cJSON_IsArray(msgs)) { cJSON_Delete(root); return 0; }

    const char *sid_raw = cjson_str(root, "sessionId");
    const char *sid = sid_raw ? hist_strndup(a, sid_raw, strlen(sid_raw)) : NULL;

    u64 emitted = 0;
    cJSON *m = NULL;
    cJSON_ArrayForEach(m, msgs) {
        const char *role_s = cjson_str(m, "role");
        if (!role_s) continue;
        bool is_user = (strcmp(role_s, "user") == 0);
        ChatRole base = is_user ? CHAT_ROLE_USER : CHAT_ROLE_ASSISTANT;
        i64 ts_ms = read_ts_ms(m);

        cJSON *content = cJSON_GetObjectItemCaseSensitive(m, "content");
        if (cJSON_IsString(content) && content->valuestring) {
            const char *t = content->valuestring;
            usize tl = strlen(t);
            if (is_user) t = unwrap_user_input(t, &tl);
            if (!emit(a, cb, user, &emitted, base, ts_ms, t, tl, NULL, sid))
                goto done;
            continue;
        }
        if (!cJSON_IsArray(content)) continue;

        cJSON *it = NULL;
        cJSON_ArrayForEach(it, content) {
            const char *type = cjson_str(it, "type");
            if (!type) continue;

            if (strcmp(type, "text") == 0) {
                const char *t = cjson_str(it, "text");
                if (!t) continue;
                usize tl = strlen(t);
                if (is_user) t = unwrap_user_input(t, &tl);
                if (!emit(a, cb, user, &emitted, base, ts_ms, t, tl, NULL, sid))
                    goto done;
            } else if (strcmp(type, "thinking") == 0) {
                /* Reasoning is part of the transcript the user is trying to
                 * re-read — the extension parser keeps its `reasoning` records
                 * for the same reason — and it is tagged assistant so the
                 * viewer renders it in the assistant's column. */
                const char *t = cjson_str(it, "thinking");
                if (!t) continue;
                if (!emit(a, cb, user, &emitted, CHAT_ROLE_ASSISTANT, ts_ms,
                          t, strlen(t), NULL, sid))
                    goto done;
            } else if (strcmp(type, "tool_use") == 0) {
                const char *name = cjson_str(it, "name");
                const char *owned_name = name
                    ? hist_strndup(a, name, strlen(name)) : NULL;
                /* The call's arguments are the useful part; render them as the
                 * event text so a tool row isn't an empty line with a label. */
                cJSON *input = cJSON_GetObjectItemCaseSensitive(it, "input");
                char *rendered = input ? cJSON_PrintUnformatted(input) : NULL;
                bool ok = emit(a, cb, user, &emitted, CHAT_ROLE_TOOL_USE, ts_ms,
                               rendered ? rendered : "", rendered ? strlen(rendered) : 0,
                               owned_name, sid);
                if (rendered) free(rendered);   /* cJSON_Print* uses malloc */
                if (!ok) goto done;
            } else if (strcmp(type, "tool_result") == 0) {
                const char *name = cjson_str(it, "name");
                const char *owned_name = name
                    ? hist_strndup(a, name, strlen(name)) : NULL;
                cJSON *c = cJSON_GetObjectItemCaseSensitive(it, "content");
                if (cJSON_IsString(c) && c->valuestring) {
                    if (!emit(a, cb, user, &emitted, CHAT_ROLE_TOOL_RESULT, ts_ms,
                              c->valuestring, strlen(c->valuestring),
                              owned_name, sid))
                        goto done;
                } else if (c) {
                    char *rendered = cJSON_PrintUnformatted(c);
                    bool ok = emit(a, cb, user, &emitted, CHAT_ROLE_TOOL_RESULT,
                                   ts_ms, rendered ? rendered : "",
                                   rendered ? strlen(rendered) : 0,
                                   owned_name, sid);
                    if (rendered) free(rendered);
                    if (!ok) goto done;
                }
            }
        }
    }

done:
    cJSON_Delete(root);
    return emitted;
}
