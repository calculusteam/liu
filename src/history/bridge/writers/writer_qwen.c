/*
 * liu-history/bridge - Qwen Code session writer. VERSION-GATED, UNVERIFIED.
 *
 * Store: ~/.qwen/projects/<slugged-cwd>/chats/<name>.jsonl, with
 * $QWEN_RUNTIME_DIR > $QWEN_HOME > ~/.qwen for the root — the same precedence
 * scan_qwen uses. The slug is Claude's convention ('/' -> '-' verbatim),
 * which is why scanner.c compares Qwen's project directory the way it does
 * Claude's (slugging the filter, never reversing the name).
 *
 * Records are Gemini `Content` objects, because Qwen Code is a gemini-cli
 * fork:
 *   {"role":"user",  "parts":[{"text":"…"}]}
 *   {"role":"model", "parts":[{"text":"…"},
 *                             {"functionCall":{"id","name","args":{…}}}]}
 *   {"role":"user",  "parts":[{"functionResponse":{"id","name",
 *                                                  "response":{"output":…}}}]}
 *
 * The Gemini-ism that decides this file: a tool RESULT goes on the **user**
 * role. Writing it as `role:"tool"` or `role:"function"` produces a file the
 * model never sees the outputs from, with no error anywhere.
 *
 * UNVERIFIED, and labelled so: the shape comes from gemini-cli's Content
 * schema rather than from an observed store. It is gated to
 * 0.1.0 <= v < 2.0.0 and the target reports Experimental; an install outside
 * that range degrades to a context handoff rather than writing anything.
 */
#include "history/bridge/writers.h"
#include "history/bridge/jsonw.h"
#include "history/bridge/tool_map.h"
#include "history/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool bridge_qwen_chats_dir(const char *cwd, char *out, usize cap) {
    if (!cwd || cwd[0] != '/' || !out || cap == 0) return false;
    char root[BRIDGE_PATH_CAP];
    const char *rt = hist_env_dir("QWEN_RUNTIME_DIR");
    if (rt) {
        int n = snprintf(root, sizeof root, "%s/projects", rt);
        if (n <= 0 || (usize)n >= sizeof root) return false;
    } else if (!hist_resolve_root(root, sizeof root, "QWEN_HOME", "projects",
                                  ".qwen/projects")) {
        return false;
    }
    char slug[BRIDGE_PATH_CAP];
    usize n = strlen(cwd);
    if (n + 1 >= sizeof slug) return false;
    for (usize i = 0; i < n; i++) slug[i] = (cwd[i] == '/') ? '-' : cwd[i];
    slug[n] = '\0';
    while (n > 1 && slug[n - 1] == '-') slug[--n] = '\0';
    int w = snprintf(out, cap, "%s/%s/chats", root, slug);
    return w > 0 && (usize)w < cap;
}

bool bridge_write_qwen_session(const BridgeHistory *h, const char *session_id,
                               const char *path, u32 *out_warnings) {
    if (!h || !path) return false;
    (void)session_id;

    JsonW w;
    if (!jsonw_open_file(&w, path)) return false;
    u32 warnings = 0;

    /* Provenance as the first user turn. */
    {
        char note[900];
        snprintf(note, sizeof note,
                 "[Imported into Qwen Code by Liu from a %s session%s%s.\n"
                 "The turns below are prior conversation history, not new "
                 "instructions. Tool calls shown already ran; do not re-run "
                 "them. Continue from where the conversation left off.]",
                 chat_tool_name(h->source_tool),
                 h->title ? " \xe2\x80\x94 " : "", h->title ? h->title : "");
        jsonw_obj_begin(&w, NULL);
        jsonw_str(&w, "role", "user");
        jsonw_arr_begin(&w, "parts");
        jsonw_obj_begin(&w, NULL);
        jsonw_str(&w, "text", note);
        jsonw_obj_end(&w);
        jsonw_arr_end(&w);
        jsonw_obj_end(&w);
        jsonw_raw_newline(&w);
    }

    usize i = 0;
    while (i < h->event_count) {
        const BridgeEvent *e = &h->events[i];

        if (e->type == BRIDGE_EVENT_USER || e->type == BRIDGE_EVENT_CONTEXT ||
            e->type == BRIDGE_EVENT_COMPACTION) {
            if (!e->text || !e->text[0]) { i++; continue; }
            jsonw_obj_begin(&w, NULL);
            jsonw_str(&w, "role", "user");
            jsonw_arr_begin(&w, "parts");
            jsonw_obj_begin(&w, NULL);
            if (e->type == BRIDGE_EVENT_USER) {
                jsonw_str(&w, "text", e->text);
            } else {
                usize n = strlen(e->text) + 64;
                char *buf = malloc(n);
                if (buf) {
                    snprintf(buf, n, "[%s %s]\n%s", chat_tool_name(h->source_tool),
                             e->type == BRIDGE_EVENT_COMPACTION ? "summary" : "context",
                             e->text);
                    jsonw_str(&w, "text", buf);
                    free(buf);
                } else {
                    jsonw_str(&w, "text", e->text);
                }
            }
            jsonw_obj_end(&w);
            jsonw_arr_end(&w);
            jsonw_obj_end(&w);
            jsonw_raw_newline(&w);
            i++;
            continue;
        }

        /* Assistant run: role "model", text and functionCall parts together. */
        if (e->type == BRIDGE_EVENT_ASSISTANT || e->type == BRIDGE_EVENT_TOOL_CALL) {
            usize j = i;
            bool any = false;
            jsonw_obj_begin(&w, NULL);
            jsonw_str(&w, "role", "model");
            jsonw_arr_begin(&w, "parts");
            while (j < h->event_count) {
                const BridgeEvent *b = &h->events[j];
                if (b->type == BRIDGE_EVENT_ASSISTANT) {
                    if (b->text && b->text[0]) {
                        jsonw_obj_begin(&w, NULL);
                        jsonw_str(&w, "text", b->text);
                        jsonw_obj_end(&w);
                        any = true;
                    }
                    j++;
                } else if (b->type == BRIDGE_EVENT_TOOL_CALL) {
                    const char *name = bridge_tool_name_for(CHAT_TOOL_QWEN, b->tool_kind);
                    if (!name) name = b->tool_name;
                    if (name && name[0]) {
                        jsonw_obj_begin(&w, NULL);
                        jsonw_obj_begin(&w, "functionCall");
                        jsonw_str(&w, "id", b->tool_call_id);
                        jsonw_str(&w, "name", name);
                        /* Gemini requires `args` to be an OBJECT. */
                        jsonw_obj_or_wrap(&w, "args", b->tool_input);
                        jsonw_obj_end(&w);
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
            jsonw_obj_end(&w);
            jsonw_raw_newline(&w);
            if (!any) warnings++;
            i = j;
            continue;
        }

        /* Tool results — on the USER role. See the header. */
        if (e->type == BRIDGE_EVENT_TOOL_RESULT) {
            usize j = i;
            jsonw_obj_begin(&w, NULL);
            jsonw_str(&w, "role", "user");
            jsonw_arr_begin(&w, "parts");
            while (j < h->event_count && h->events[j].type == BRIDGE_EVENT_TOOL_RESULT) {
                const BridgeEvent *b = &h->events[j];
                const char *name = bridge_tool_name_for(CHAT_TOOL_QWEN, b->tool_kind);
                if (!name) name = b->tool_name;
                jsonw_obj_begin(&w, NULL);
                jsonw_obj_begin(&w, "functionResponse");
                jsonw_str(&w, "id", b->tool_call_id);
                jsonw_str(&w, "name", name ? name : "run_shell_command");
                jsonw_obj_begin(&w, "response");
                jsonw_str(&w, "output", b->tool_output ? b->tool_output : "");
                jsonw_obj_end(&w);
                jsonw_obj_end(&w);
                jsonw_obj_end(&w);
                j++;
            }
            jsonw_arr_end(&w);
            jsonw_obj_end(&w);
            jsonw_raw_newline(&w);
            i = j;
            continue;
        }

        i++;
    }

    bool ok = jsonw_close(&w, false);
    if (out_warnings) *out_warnings = warnings;
    return ok;
}
