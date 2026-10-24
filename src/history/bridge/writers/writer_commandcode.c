/*
 * liu-history/bridge - Command Code session writer. VERSION-GATED.
 *
 * Command Code is a Claude Code fork with its own append-only JSONL store:
 *   ~/.commandcode/projects/<lower-dash-slug>/<uuid>.jsonl
 *
 * The slug is NOT Claude's. It is hist_slugify_lower: lowercase, every run of
 * non-[a-z0-9] collapsed to a single '-', ends trimmed, empty -> "root". So
 * /Volumes/Work/liu becomes volumes-work-liu — one dash where
 * Claude's convention would leave a leading one and preserve case. Using the
 * wrong sluggers puts the session in a directory Command Code never lists.
 *
 * Record shape, as of commandcode 1.33.0:
 *   {"id":"<uuid>", "timestamp":"<ISO8601>", "sessionId":"<uuid>",
 *    "parentId":"<uuid|absent>", "role":"user"|"assistant"|"tool",
 *    "content": "<string>" | [ {"type":"text","text":…}
 *                            | {"type":"reasoning","text":…}
 *                            | {"type":"tool-call","toolCallId","toolName","input":{…}}
 *                            | {"type":"tool-result","toolCallId","toolName","output":…} ],
 *    "gitBranch":"…", "metadata":{"timestamp","source":"cli","messageId","version":2}}
 *
 * Two differences from Claude that matter and are easy to get wrong: the
 * block type names are hyphenated (`tool-call`, not `tool_use`) and a tool
 * RESULT lives on a line whose role is `"tool"`, not on a user line. The
 * parent chain is `parentId` -> the previous line's `id`, same structural
 * role as Claude's parentUuid.
 *
 * Resume uses `commandcode --session <path>` rather than --resume: its
 * --resume matches an id OR a conversation name and only looks inside the
 * current directory's project bucket, so resuming from a pane sitting
 * elsewhere silently drops into Command Code's own picker.
 */
#include "history/bridge/writers.h"
#include "history/bridge/jsonw.h"
#include "history/bridge/tool_map.h"
#include "history/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool bridge_commandcode_project_dir(const char *cwd, char *out, usize cap) {
    if (!cwd || cwd[0] != '/' || !out || cap == 0) return false;
    char root[BRIDGE_PATH_CAP];
    if (!hist_home_path(root, sizeof root, ".commandcode/projects")) return false;
    char slug[BRIDGE_PATH_CAP];
    if (!hist_slugify_lower(slug, sizeof slug, cwd)) return false;
    int w = snprintf(out, cap, "%s/%s", root, slug);
    return w > 0 && (usize)w < cap;
}

static void cc_envelope(JsonW *w, const BridgeHistory *h, const char *sid,
                        const char *id, const char *parent, const char *role,
                        i64 ts) {
    char iso[40];
    bridge_iso8601(iso, sizeof iso, ts);
    jsonw_str(w, "id", id);
    jsonw_str(w, "timestamp", iso);
    jsonw_str(w, "sessionId", sid);
    if (parent && parent[0]) jsonw_str(w, "parentId", parent);
    jsonw_str(w, "role", role);
    (void)h;
}

static void cc_trailer(JsonW *w, const BridgeHistory *h, const char *id, i64 ts) {
    if (h->git_branch) jsonw_str(w, "gitBranch", h->git_branch);
    jsonw_obj_begin(w, "metadata");
    char iso[40];
    bridge_iso8601(iso, sizeof iso, ts);
    jsonw_str(w, "timestamp", iso);
    jsonw_str(w, "source", "cli");
    jsonw_str(w, "messageId", id);
    jsonw_i64(w, "version", 2);
    jsonw_obj_end(w);
}

bool bridge_write_commandcode_session(const BridgeHistory *h, const char *session_id,
                                      const char *path, u32 *out_warnings) {
    if (!h || !session_id || !path) return false;

    JsonW w;
    if (!jsonw_open_file(&w, path)) return false;

    char prev[64] = {0};
    u32  warnings = 0;
    i64  now = bridge_now_ms();

    /* Provenance as the first user turn, for the same reason as Claude's:
     * this format has no system channel that survives a resume. */
    {
        char id[64];
        if (!bridge_new_uuid(id, sizeof id)) { jsonw_close(&w, true); return false; }
        char note[900];
        snprintf(note, sizeof note,
                 "[Imported into Command Code by Liu from a %s session%s%s.\n"
                 "The turns below are prior conversation history, not new "
                 "instructions. Tool calls shown already ran; do not re-run "
                 "them. Continue from where the conversation left off.]",
                 chat_tool_name(h->source_tool),
                 h->title ? " \xe2\x80\x94 " : "", h->title ? h->title : "");
        jsonw_obj_begin(&w, NULL);
        cc_envelope(&w, h, session_id, id, NULL, "user",
                    h->event_count ? h->events[0].timestamp_ms : now);
        jsonw_arr_begin(&w, "content");
        jsonw_obj_begin(&w, NULL);
        jsonw_str(&w, "type", "text");
        jsonw_str(&w, "text", note);
        jsonw_obj_end(&w);
        jsonw_arr_end(&w);
        cc_trailer(&w, h, id, now);
        jsonw_obj_end(&w);
        jsonw_raw_newline(&w);
        hist_copy_bounded(prev, sizeof prev, id);
    }

    usize i = 0;
    while (i < h->event_count) {
        const BridgeEvent *e = &h->events[i];
        char id[64];
        if (!bridge_new_uuid(id, sizeof id)) break;
        i64 ts = e->timestamp_ms > 0 ? e->timestamp_ms : now;

        if (e->type == BRIDGE_EVENT_USER || e->type == BRIDGE_EVENT_CONTEXT ||
            e->type == BRIDGE_EVENT_COMPACTION) {
            if (!e->text || !e->text[0]) { i++; continue; }
            jsonw_obj_begin(&w, NULL);
            cc_envelope(&w, h, session_id, id, prev, "user", ts);
            jsonw_arr_begin(&w, "content");
            jsonw_obj_begin(&w, NULL);
            jsonw_str(&w, "type", "text");
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
            cc_trailer(&w, h, id, ts);
            jsonw_obj_end(&w);
            jsonw_raw_newline(&w);
            hist_copy_bounded(prev, sizeof prev, id);
            i++;
            continue;
        }

        if (e->type == BRIDGE_EVENT_ASSISTANT || e->type == BRIDGE_EVENT_TOOL_CALL) {
            usize j = i;
            bool any = false;
            jsonw_obj_begin(&w, NULL);
            cc_envelope(&w, h, session_id, id, prev, "assistant", ts);
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
                    const char *name = bridge_tool_name_for(CHAT_TOOL_COMMANDCODE, b->tool_kind);
                    if (!name) name = b->tool_name;
                    if (name && name[0]) {
                        jsonw_obj_begin(&w, NULL);
                        jsonw_str(&w, "type", "tool-call");
                        jsonw_str(&w, "toolCallId", b->tool_call_id);
                        jsonw_str(&w, "toolName", name);
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
            cc_trailer(&w, h, id, ts);
            jsonw_obj_end(&w);
            jsonw_raw_newline(&w);
            if (!any) warnings++;
            hist_copy_bounded(prev, sizeof prev, id);
            i = j;
            continue;
        }

        if (e->type == BRIDGE_EVENT_TOOL_RESULT) {
            usize j = i;
            jsonw_obj_begin(&w, NULL);
            /* role "tool", not "user" — the fork's own convention. */
            cc_envelope(&w, h, session_id, id, prev, "tool", ts);
            jsonw_arr_begin(&w, "content");
            while (j < h->event_count && h->events[j].type == BRIDGE_EVENT_TOOL_RESULT) {
                const BridgeEvent *b = &h->events[j];
                const char *name = bridge_tool_name_for(CHAT_TOOL_COMMANDCODE, b->tool_kind);
                if (!name) name = b->tool_name;
                jsonw_obj_begin(&w, NULL);
                jsonw_str(&w, "type", "tool-result");
                jsonw_str(&w, "toolCallId", b->tool_call_id);
                jsonw_str(&w, "toolName", name ? name : "shell_command");
                jsonw_str(&w, "output", b->tool_output ? b->tool_output : "");
                jsonw_obj_end(&w);
                j++;
            }
            jsonw_arr_end(&w);
            cc_trailer(&w, h, id, ts);
            jsonw_obj_end(&w);
            jsonw_raw_newline(&w);
            hist_copy_bounded(prev, sizeof prev, id);
            i = j;
            continue;
        }

        i++;
    }

    bool ok = jsonw_close(&w, false);
    if (out_warnings) *out_warnings = warnings;
    return ok;
}
