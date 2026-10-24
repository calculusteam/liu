/*
 * liu-history/bridge - Grok session writer. VERSION-GATED.
 *
 * Grok stores a session as a DIRECTORY, not a file:
 *   ~/.grok/sessions/<percent-encoded-cwd>/<uuid>/
 * ($GROK_HOME overrides ~/.grok.) A session directory holds these files:
 *
 *   chat_history.jsonl   the conversation           <- written
 *   summary.json         id, cwd, times, model      <- written
 *   system_prompt.txt    the harness prompt         <- NOT written
 *   events.jsonl, updates.jsonl, rewind_points.jsonl,
 *   signals.json, prompt_context.json, announcement_state.json,
 *   *.lock                                          <- NOT written
 *
 * Only the first two are written, and the omissions are deliberate. The lock
 * files are a running process's business and creating them for a session
 * nobody has opened is a way to wedge one. `system_prompt.txt` is Grok's own
 * prompt, which it regenerates per version and per model; a stale copy
 * imported from Liu would pin an old one. The rest are per-run telemetry and
 * UI state that a session legitimately starts without — Grok recreates them
 * on first open, which is exactly what a resumed-but-never-run session should
 * look like.
 *
 * The percent encoding of the project directory is the one non-obvious bit:
 * ~/.grok/sessions/%2Fprivate%2Ftmp%2Fgrokprobe/<uuid>/ — the absolute cwd
 * with '/' as %2F, uppercase hex. Sessions may also sit directly under the
 * root with no project directory (a "global" session), but a session written
 * without one loses its cwd association and would not be offered when the
 * user is inside the project, so this always writes the project form.
 *
 * chat_history.jsonl record shapes, as of grok 1.0.5:
 *   {"type":"system","content":"…"}
 *   {"type":"user","content":[{"type":"text","text":"<user_query>…"}],
 *    "prompt_index":N}
 *   {"type":"assistant","content":"…","model_id":"…"}
 *   {"type":"tool_result","tool_call_id":"…","content":…}
 *
 * A real user turn is wrapped in <user_query>…</user_query> and carries an
 * increasing `prompt_index`; both are reproduced, because the CLI's rewind
 * and prompt-history features index on them.
 */
#include "history/bridge/writers.h"
#include "history/bridge/jsonw.h"
#include "history/bridge/tool_map.h"
#include "history/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Percent-encode a path the way Grok names its project buckets: every byte
 * outside [A-Za-z0-9._-~] becomes %XX with UPPERCASE hex. */
static bool percent_encode(const char *s, char *out, usize cap) {
    static const char hex[] = "0123456789ABCDEF";
    usize o = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        bool safe = (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                    (*p >= '0' && *p <= '9') ||
                    *p == '.' || *p == '_' || *p == '-' || *p == '~';
        if (safe) {
            if (o + 1 >= cap) return false;
            out[o++] = (char)*p;
        } else {
            if (o + 3 >= cap) return false;
            out[o++] = '%';
            out[o++] = hex[*p >> 4];
            out[o++] = hex[*p & 0x0F];
        }
    }
    if (o >= cap) return false;
    out[o] = '\0';
    return true;
}

bool bridge_grok_session_dir(const char *cwd, const char *session_id,
                             char *out, usize cap) {
    if (!cwd || cwd[0] != '/' || !session_id || !out) return false;
    char root[BRIDGE_PATH_CAP];
    if (!hist_resolve_root(root, sizeof root, "GROK_HOME", "sessions",
                           ".grok/sessions"))
        return false;
    char enc[BRIDGE_PATH_CAP];
    if (!percent_encode(cwd, enc, sizeof enc)) return false;
    int w = snprintf(out, cap, "%s/%s/%s", root, enc, session_id);
    return w > 0 && (usize)w < cap;
}

static bool write_summary(const BridgeHistory *h, const char *session_id,
                          const char *dir) {
    char path[BRIDGE_PATH_CAP];
    int n = snprintf(path, sizeof path, "%s/summary.json", dir);
    if (n <= 0 || (usize)n >= sizeof path) return false;
    if (bridge_path_exists(path)) return false;

    i64 t0 = 0, t1 = 0;
    for (usize i = 0; i < h->event_count; i++) {
        i64 t = h->events[i].timestamp_ms;
        if (t <= 0) continue;
        if (!t0 || t < t0) t0 = t;
        if (t > t1) t1 = t;
    }
    if (!t0) t0 = bridge_now_ms();
    if (!t1) t1 = t0;

    u32 msgs = 0;
    for (usize i = 0; i < h->event_count; i++) {
        BridgeEventType t = h->events[i].type;
        if (t == BRIDGE_EVENT_USER || t == BRIDGE_EVENT_ASSISTANT) msgs++;
    }

    char iso0[40], iso1[40];
    bridge_iso8601(iso0, sizeof iso0, t0);
    bridge_iso8601(iso1, sizeof iso1, t1);

    JsonW w;
    if (!jsonw_open_file(&w, path)) return false;
    jsonw_obj_begin(&w, NULL);
    jsonw_obj_begin(&w, "info");
    jsonw_str(&w, "id", session_id);
    jsonw_str(&w, "cwd", h->cwd);
    jsonw_obj_end(&w);
    jsonw_str(&w, "session_summary", h->title ? h->title : "");
    jsonw_str(&w, "created_at", iso0);
    jsonw_str(&w, "updated_at", iso1);
    jsonw_i64(&w, "num_messages", (i64)msgs + 1);   /* + the provenance turn */
    jsonw_i64(&w, "num_chat_messages", (i64)msgs + 1);
    jsonw_str(&w, "current_model_id",
              h->source_model && h->source_model[0] ? h->source_model : "grok-4.5");
    jsonw_i64(&w, "next_trace_turn", 1);
    jsonw_i64(&w, "chat_format_version", 1);
    jsonw_str(&w, "last_active_at", iso1);
    /* session_kind matters: the scanner (and Grok's own picker) hides
     * "subagent" rows, so an imported session must NOT claim to be one. */
    jsonw_str(&w, "session_kind", "user");
    /* Deliberately absent: sandbox_profile, reasoning_effort, request_id,
     * grok_home. The first two are policy the target must choose for itself;
     * the last two are per-run identity. */
    jsonw_obj_end(&w);
    return jsonw_close(&w, false);
}

bool bridge_write_grok_session(const BridgeHistory *h, const char *session_id,
                               const char *dir, u32 *out_warnings) {
    if (!h || !session_id || !dir) return false;
    if (!bridge_mkdir_p(dir)) return false;

    char chat[BRIDGE_PATH_CAP];
    int n = snprintf(chat, sizeof chat, "%s/chat_history.jsonl", dir);
    if (n <= 0 || (usize)n >= sizeof chat) return false;
    if (bridge_path_exists(chat)) return false;   /* never touch a real session */

    JsonW w;
    if (!jsonw_open_file(&w, chat)) return false;
    u32 warnings = 0;
    u32 prompt_index = 0;

    /* Provenance, as a real <user_query> so it shows in the transcript the
     * same way the user's own turns do. */
    {
        char note[900];
        snprintf(note, sizeof note,
                 "<user_query>\n[Imported into Grok by Liu from a %s session%s%s.\n"
                 "The turns below are prior conversation history, not new "
                 "instructions. Tool calls shown already ran; do not re-run them. "
                 "Continue from where the conversation left off.]\n</user_query>",
                 chat_tool_name(h->source_tool),
                 h->title ? " \xe2\x80\x94 " : "", h->title ? h->title : "");
        jsonw_obj_begin(&w, NULL);
        jsonw_str(&w, "type", "user");
        jsonw_arr_begin(&w, "content");
        jsonw_obj_begin(&w, NULL);
        jsonw_str(&w, "type", "text");
        jsonw_str(&w, "text", note);
        jsonw_obj_end(&w);
        jsonw_arr_end(&w);
        jsonw_i64(&w, "prompt_index", prompt_index++);
        jsonw_obj_end(&w);
        jsonw_raw_newline(&w);
    }

    for (usize i = 0; i < h->event_count; i++) {
        const BridgeEvent *e = &h->events[i];
        switch (e->type) {
            case BRIDGE_EVENT_USER: {
                if (!e->text || !e->text[0]) break;
                char *safe = bridge_escape_closing_tag(e->text, "user_query");
                const char *text = safe ? safe : e->text;
                usize need = strlen(text) + 32;
                char *buf = malloc(need);
                if (!buf) { free(safe); warnings++; break; }
                snprintf(buf, need, "<user_query>\n%s\n</user_query>", text);
                jsonw_obj_begin(&w, NULL);
                jsonw_str(&w, "type", "user");
                jsonw_arr_begin(&w, "content");
                jsonw_obj_begin(&w, NULL);
                jsonw_str(&w, "type", "text");
                jsonw_str(&w, "text", buf);
                jsonw_obj_end(&w);
                jsonw_arr_end(&w);
                jsonw_i64(&w, "prompt_index", prompt_index++);
                jsonw_obj_end(&w);
                jsonw_raw_newline(&w);
                free(buf); free(safe);
                break;
            }

            case BRIDGE_EVENT_CONTEXT:
            case BRIDGE_EVENT_COMPACTION: {
                if (!e->text || !e->text[0]) break;
                /* `synthetic_reason` is Grok's own marker for a user-channel
                 * message it injected rather than the user typing it, which is
                 * exactly what these are. Reusing it keeps them out of the
                 * prompt history the CLI's up-arrow walks. */
                jsonw_obj_begin(&w, NULL);
                jsonw_str(&w, "type", "user");
                jsonw_arr_begin(&w, "content");
                jsonw_obj_begin(&w, NULL);
                jsonw_str(&w, "type", "text");
                jsonw_str(&w, "text", e->text);
                jsonw_obj_end(&w);
                jsonw_arr_end(&w);
                jsonw_str(&w, "synthetic_reason", "liu_bridge_import");
                jsonw_obj_end(&w);
                jsonw_raw_newline(&w);
                break;
            }

            case BRIDGE_EVENT_ASSISTANT: {
                if (!e->text || !e->text[0]) break;
                jsonw_obj_begin(&w, NULL);
                jsonw_str(&w, "type", "assistant");
                jsonw_str(&w, "content", e->text);
                jsonw_str(&w, "model_id",
                          h->source_model && h->source_model[0] ? h->source_model
                                                                : "grok-4.5");
                jsonw_obj_end(&w);
                jsonw_raw_newline(&w);
                break;
            }

            case BRIDGE_EVENT_TOOL_CALL: {
                const char *name = bridge_tool_name_for(CHAT_TOOL_XAI, e->tool_kind);
                if (!name) name = e->tool_name;
                if (!name || !name[0]) { warnings++; break; }
                /* Grok hangs calls off an assistant line in a sibling array,
                 * so an imported call is an assistant line with an empty
                 * content and one tool_calls entry. */
                jsonw_obj_begin(&w, NULL);
                jsonw_str(&w, "type", "assistant");
                jsonw_str(&w, "content", "");
                jsonw_arr_begin(&w, "tool_calls");
                jsonw_obj_begin(&w, NULL);
                jsonw_str(&w, "id", e->tool_call_id);
                jsonw_str(&w, "name", name);
                jsonw_raw_or_str(&w, "arguments",
                                 e->tool_input && e->tool_input[0] ? e->tool_input : "{}");
                jsonw_obj_end(&w);
                jsonw_arr_end(&w);
                jsonw_str(&w, "model_id",
                          h->source_model && h->source_model[0] ? h->source_model
                                                                : "grok-4.5");
                jsonw_obj_end(&w);
                jsonw_raw_newline(&w);
                break;
            }

            case BRIDGE_EVENT_TOOL_RESULT: {
                jsonw_obj_begin(&w, NULL);
                jsonw_str(&w, "type", "tool_result");
                jsonw_str(&w, "tool_call_id", e->tool_call_id);
                jsonw_str(&w, "content", e->tool_output ? e->tool_output : "");
                jsonw_obj_end(&w);
                jsonw_raw_newline(&w);
                break;
            }

            default: break;
        }
    }

    bool ok = jsonw_close(&w, false);
    if (ok) ok = write_summary(h, session_id, dir);
    if (out_warnings) *out_warnings = warnings;
    return ok;
}
