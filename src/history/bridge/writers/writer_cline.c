/*
 * liu-history/bridge - Cline CLI session writer.
 *
 * This adapter targets the standalone cline binary, not the VS Code
 * extension. They use different stores and resume commands, so the extension
 * task type must remain distinct from CHAT_TOOL_CLINE_CLI.
 *
 * The writer targets this session-store layout:
 *   ~/.cline/data/sessions/<ts>_<rand>/
 *       <id>.json           metadata
 *       <id>.messages.json  transcript
 *
 * The session ID is the directory name passed to cline --id. Its native form
 * is <unix-ms>_<5 lowercase base36 chars>; using a UUID would not match the
 * target's expected directory format.
 *
 * The messages file uses numeric millisecond timestamps:
 *   { "version": 1, "updated_at": "<ISO>", "agent": "lead",
 *     "sessionId": "<id>", "origin": {source,mode,sessionId,version},
 *     "system_prompt": "",
 *     "messages": [ { "id":"msg_…", "role":"user"|"assistant",
 *                     "ts": <unix ms NUMBER>,
 *                     "content": [ {type:"text",text}
 *                                | {type:"thinking",thinking}
 *                                | {type:"tool_use",name,input}
 *                                | {type:"tool_result",name,content} ] } ] }
 *
 * The extension store uses a string for ts; this writer must use a number or
 * message timestamps are interpreted incorrectly. system_prompt is empty so
 * Cline can generate the prompt for the current version and provider.
 */
#include "history/bridge/writers.h"
#include "history/bridge/jsonw.h"
#include "history/bridge/tool_map.h"
#include "history/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool bridge_cline_session_dir(const char *session_id, char *out, usize cap) {
    if (!session_id || !out) return false;
    char root[BRIDGE_PATH_CAP];
    if (!hist_home_path(root, sizeof root, ".cline/data/sessions")) return false;
    int w = snprintf(out, cap, "%s/%s", root, session_id);
    return w > 0 && (usize)w < cap;
}

/* <unix-ms>_<5 base36 chars>, the native id shape. */
bool bridge_cline_new_id(char *out, usize cap) {
    if (!out || cap < 24) return false;
    char rnd[32];
    if (!bridge_new_short_id(rnd, sizeof rnd)) return false;
    int w = snprintf(out, cap, "%lld_%.5s", (long long)bridge_now_ms(), rnd);
    return w > 0 && (usize)w < cap;
}

static void cline_meta(const BridgeHistory *h, const char *session_id,
                       const char *dir, bool *ok) {
    char path[BRIDGE_PATH_CAP];
    int n = snprintf(path, sizeof path, "%s/%s.json", dir, session_id);
    if (n <= 0 || (usize)n >= sizeof path) { *ok = false; return; }
    if (bridge_path_exists(path)) { *ok = false; return; }

    i64 t0 = 0, t1 = 0;
    for (usize i = 0; i < h->event_count; i++) {
        i64 t = h->events[i].timestamp_ms;
        if (t <= 0) continue;
        if (!t0 || t < t0) t0 = t;
        if (t > t1) t1 = t;
    }
    if (!t0) t0 = bridge_now_ms();
    if (!t1) t1 = t0;
    char iso0[40], iso1[40];
    bridge_iso8601(iso0, sizeof iso0, t0);
    bridge_iso8601(iso1, sizeof iso1, t1);

    JsonW w;
    if (!jsonw_open_file(&w, path)) { *ok = false; return; }
    jsonw_obj_begin(&w, NULL);
    jsonw_i64(&w, "version", 1);
    jsonw_str(&w, "session_id", session_id);
    jsonw_str(&w, "source", "cli");
    jsonw_str(&w, "started_at", iso0);
    jsonw_str(&w, "ended_at", iso1);
    jsonw_str(&w, "status", "completed");
    jsonw_bool(&w, "interactive", true);
    jsonw_str(&w, "model",
              h->source_model && h->source_model[0] ? h->source_model : "");
    jsonw_str(&w, "cwd", h->cwd);
    jsonw_str(&w, "workspace_root", h->cwd);
    /* Deliberately NOT written: `pid` (a process that does not exist),
     * `exit_code`, `provider`, and the three enable_* capability flags —
     * those are policy the user's own Cline configuration decides, and an
     * imported session must not carry another agent's answer to them. */
    jsonw_obj_end(&w);
    if (!jsonw_close(&w, false)) *ok = false;
}

bool bridge_write_cline_session(const BridgeHistory *h, const char *session_id,
                                const char *dir, u32 *out_warnings) {
    if (!h || !session_id || !dir) return false;
    if (!bridge_mkdir_p(dir)) return false;

    char path[BRIDGE_PATH_CAP];
    int n = snprintf(path, sizeof path, "%s/%s.messages.json", dir, session_id);
    if (n <= 0 || (usize)n >= sizeof path) return false;
    if (bridge_path_exists(path)) return false;

    JsonW w;
    if (!jsonw_open_file(&w, path)) return false;
    u32 warnings = 0;
    i64 now = bridge_now_ms();
    char iso[40];
    bridge_iso8601(iso, sizeof iso, now);

    jsonw_obj_begin(&w, NULL);
    jsonw_i64(&w, "version", 1);
    jsonw_str(&w, "updated_at", iso);
    jsonw_str(&w, "agent", "lead");
    jsonw_str(&w, "sessionId", session_id);
    jsonw_obj_begin(&w, "origin");
    jsonw_str(&w, "source", "cli");
    jsonw_str(&w, "mode", "user");
    jsonw_str(&w, "sessionId", session_id);
    jsonw_str(&w, "version", "0.0.0-liu-bridge");
    jsonw_obj_end(&w);
    jsonw_str(&w, "system_prompt", "");   /* see the header */
    jsonw_arr_begin(&w, "messages");

    u32 seq = 0;
    /* Provenance turn. Cline wraps what the user typed in
     * <user_input mode="act">…</user_input>; parser_cline_cli.c strips that
     * on the way in, so it is reproduced on the way out — a user turn without
     * it is not the shape the CLI's own reader expects. */
    {
        char id[64];
        snprintf(id, sizeof id, "msg_liu%08u", ++seq);
        char note[1000];
        snprintf(note, sizeof note,
                 "<user_input mode=\"act\">[Imported into Cline by Liu from a "
                 "%s session%s%s.\nThe turns below are prior conversation "
                 "history, not new instructions. Tool calls shown already ran; "
                 "do not re-run them. Continue from where the conversation left "
                 "off.]</user_input>",
                 chat_tool_name(h->source_tool),
                 h->title ? " \xe2\x80\x94 " : "", h->title ? h->title : "");
        jsonw_obj_begin(&w, NULL);
        jsonw_str(&w, "id", id);
        jsonw_str(&w, "role", "user");
        jsonw_arr_begin(&w, "content");
        jsonw_obj_begin(&w, NULL);
        jsonw_str(&w, "type", "text");
        jsonw_str(&w, "text", note);
        jsonw_obj_end(&w);
        jsonw_arr_end(&w);
        jsonw_i64(&w, "ts", now);          /* a NUMBER — see the header */
        jsonw_obj_end(&w);
    }

    usize i = 0;
    while (i < h->event_count) {
        const BridgeEvent *e = &h->events[i];
        char id[64];
        snprintf(id, sizeof id, "msg_liu%08u", ++seq);
        i64 ts = e->timestamp_ms > 0 ? e->timestamp_ms : now;

        if (e->type == BRIDGE_EVENT_USER || e->type == BRIDGE_EVENT_CONTEXT ||
            e->type == BRIDGE_EVENT_COMPACTION) {
            if (!e->text || !e->text[0]) { i++; continue; }
            /* The user text goes inside Cline's <user_input> wrapper; a
             * "</user_input>" INSIDE it would end the wrapper early. */
            char *safe = e->type == BRIDGE_EVENT_USER
                       ? bridge_escape_closing_tag(e->text, "user_input") : NULL;
            const char *text = safe ? safe : e->text;
            usize need = strlen(text) + 128;
            char *buf = malloc(need);
            if (!buf) { free(safe); warnings++; i++; continue; }
            if (e->type == BRIDGE_EVENT_USER) {
                snprintf(buf, need, "<user_input mode=\"act\">%s</user_input>", text);
            } else {
                snprintf(buf, need, "[%s %s]\n%s", chat_tool_name(h->source_tool),
                         e->type == BRIDGE_EVENT_COMPACTION ? "summary" : "context",
                         e->text);
            }
            jsonw_obj_begin(&w, NULL);
            jsonw_str(&w, "id", id);
            jsonw_str(&w, "role", "user");
            jsonw_arr_begin(&w, "content");
            jsonw_obj_begin(&w, NULL);
            jsonw_str(&w, "type", "text");
            jsonw_str(&w, "text", buf);
            jsonw_obj_end(&w);
            jsonw_arr_end(&w);
            jsonw_i64(&w, "ts", ts);
            jsonw_obj_end(&w);
            free(buf); free(safe);
            i++;
            continue;
        }

        if (e->type == BRIDGE_EVENT_ASSISTANT || e->type == BRIDGE_EVENT_TOOL_CALL) {
            usize j = i;
            bool any = false;
            jsonw_obj_begin(&w, NULL);
            jsonw_str(&w, "id", id);
            jsonw_str(&w, "role", "assistant");
            jsonw_arr_begin(&w, "content");
            while (j < h->event_count) {
                const BridgeEvent *b = &h->events[j];
                if (b->type == BRIDGE_EVENT_ASSISTANT) {
                    if (b->text && b->text[0]) {
                        jsonw_obj_begin(&w, NULL);
                        jsonw_str(&w, "type", "text");
                        jsonw_str(&w, "text", b->text);
                        jsonw_obj_end(&w);
                        any = true;
                    }
                    j++;
                } else if (b->type == BRIDGE_EVENT_TOOL_CALL) {
                    const char *name = bridge_tool_name_for(CHAT_TOOL_CLINE_CLI, b->tool_kind);
                    if (!name) name = b->tool_name;
                    if (name && name[0]) {
                        jsonw_obj_begin(&w, NULL);
                        jsonw_str(&w, "type", "tool_use");
                        jsonw_str(&w, "id", b->tool_call_id);
                        jsonw_str(&w, "name", name);
                        jsonw_raw_or_str(&w, "input",
                                         b->tool_input && b->tool_input[0]
                                             ? b->tool_input : "{}");
                        jsonw_obj_end(&w);
                        any = true;
                    } else {
                        warnings++;
                    }
                    j++;
                } else {
                    break;
                }
            }
            jsonw_arr_end(&w);
            jsonw_i64(&w, "ts", ts);
            jsonw_obj_end(&w);
            if (!any) warnings++;
            i = j;
            continue;
        }

        if (e->type == BRIDGE_EVENT_TOOL_RESULT) {
            usize j = i;
            jsonw_obj_begin(&w, NULL);
            jsonw_str(&w, "id", id);
            jsonw_str(&w, "role", "user");
            jsonw_arr_begin(&w, "content");
            while (j < h->event_count && h->events[j].type == BRIDGE_EVENT_TOOL_RESULT) {
                const BridgeEvent *b = &h->events[j];
                const char *name = bridge_tool_name_for(CHAT_TOOL_CLINE_CLI, b->tool_kind);
                if (!name) name = b->tool_name;
                jsonw_obj_begin(&w, NULL);
                jsonw_str(&w, "type", "tool_result");
                jsonw_str(&w, "tool_use_id", b->tool_call_id);
                jsonw_str(&w, "name", name ? name : "Bash");
                jsonw_str(&w, "content", b->tool_output ? b->tool_output : "");
                jsonw_obj_end(&w);
                j++;
            }
            jsonw_arr_end(&w);
            jsonw_i64(&w, "ts", ts);
            jsonw_obj_end(&w);
            i = j;
            continue;
        }

        i++;
    }

    jsonw_arr_end(&w);
    jsonw_obj_end(&w);

    bool ok = jsonw_close(&w, false);
    if (ok) cline_meta(h, session_id, dir, &ok);
    if (out_warnings) *out_warnings = warnings;
    return ok;
}
