/*
 * liu-history/bridge - Claude Code session writer. VERSION-GATED.
 *
 * Claude Code resumes its OWN sessions but has no public API for seeding an
 * arbitrary one, so the only route in is its on-disk transcript.
 * That makes this the most fragile adapter in the tree, and the reason every
 * byte of Claude serialization lives in exactly this file: when the format
 * moves, one file changes.
 *
 * Layout, as of claude 2.1.248:
 *   ~/.claude/projects/<slugged-cwd>/<session-uuid>.jsonl
 * where <slugged-cwd> is the absolute cwd with every '/' replaced by '-'
 * (so /Volumes/Work/liu -> -Volumes-Work-liu) and the file's
 * basename IS the session id `claude --resume` takes.
 *
 * The record shape, per line:
 *   {"parentUuid": <uuid|null>, "isSidechain": false, "userType": "external",
 *    "cwd": "...", "sessionId": "...", "version": "...", "gitBranch": "...",
 *    "type": "user"|"assistant", "message": {...},
 *    "uuid": "...", "timestamp": "<ISO8601 Z>"}
 *
 *   user      message = {role:"user", content: <string> | [tool_result blocks]}
 *   assistant message = {role:"assistant", id, model, type:"message",
 *                        stop_reason, stop_sequence, usage,
 *                        content:[text|tool_use blocks]}
 *
 * The single load-bearing property is the **parentUuid chain**. Claude does
 * not read the file as a flat list — it walks backwards from a leaf through
 * parentUuid to reconstruct the conversation, which is what makes forking and
 * rewinding work. A file whose chain is broken resumes as a *fragment*
 * starting at the first unreachable link, with no error: the user sees a
 * session that silently forgot its beginning. So every line here records the
 * uuid of the line before it, and the first line records null.
 *
 * Text the model is to read but the user did not say — the note that the
 * conversation was imported, a source's injected context — is an `isMeta`
 * user line: Claude sends it to the model and does not draw it. A source's
 * summary of dropped history is Claude's own compact summary line. Tool calls
 * are written as Claude's own tools with Claude's arguments (tool_args.h), and
 * each result on a line of its own, an edit's with the `toolUseResult` Claude
 * draws its diff from.
 *
 * Deliberately NOT written: `thinking` blocks (another model's private
 * channel), `file-history-snapshot` / `file-history-delta`
 * (they reference backup blobs that do not exist for an imported session, and
 * a dangling reference is worse than an absent one), and any of the
 * permission / mode records — an imported session must start under whatever
 * policy the user's Claude is configured with, never one carried across.
 */
#include "history/bridge/writers.h"
#include "history/bridge/jsonw.h"
#include "history/bridge/tool_args.h"
#include "history/bridge/tool_map.h"
#include "history/util.h"

#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ISO 8601 with milliseconds and a Z, which is what every line in a real
 * transcript carries and what hist_parse_iso8601_ms reads back. */
static void iso8601(char *out, usize cap, i64 ms) {
    if (ms <= 0) ms = bridge_now_ms();
    time_t secs = (time_t)(ms / 1000);
    struct tm tmv;
    gmtime_r(&secs, &tmv);
    snprintf(out, cap, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec, (int)(ms % 1000));
}

bool bridge_claude_project_slug(const char *cwd, char *out, usize cap) {
    if (!cwd || cwd[0] != '/' || !out || cap == 0) return false;
    /* Claude's own rule (hist_slugify_claude): every byte that is not
     * [A-Za-z0-9] becomes '-'. Turning only '/' into '-' would put a
     * session for "/w/My App.v2" in a directory Claude never looks in for
     * that cwd, and `claude --resume` could not find it. */
    char trimmed[BRIDGE_PATH_CAP];
    usize n = strlen(cwd);
    if (n + 1 >= sizeof trimmed) return false;
    memcpy(trimmed, cwd, n + 1);
    /* A trailing slash is the same directory (and would be a trailing dash,
     * splitting one project into two). */
    while (n > 1 && trimmed[n - 1] == '/') trimmed[--n] = '\0';
    return hist_slugify_claude(out, cap, trimmed);
}

bool bridge_claude_project_dir(const char *cwd, char *out, usize cap) {
    if (!cwd || cwd[0] != '/' || !out || cap == 0) return false;
    char root[BRIDGE_PATH_CAP];
    /* $CLAUDE_CONFIG_DIR overrides ~/.claude, the same precedence the scanner
     * uses — a user who moved their config must not have sessions written to
     * a directory Claude never reads. */
    const char *env = hist_env_dir("CLAUDE_CONFIG_DIR");
    if (env) {
        int n = snprintf(root, sizeof root, "%s/projects", env);
        if (n <= 0 || (usize)n >= sizeof root) return false;
    } else if (!hist_home_path(root, sizeof root, ".claude/projects")) {
        return false;
    }

    char slug[BRIDGE_PATH_CAP];
    if (!bridge_claude_project_slug(cwd, slug, sizeof slug)) return false;
    int w = snprintf(out, cap, "%s/%s", root, slug);
    return w > 0 && (usize)w < cap;
}

/* Envelope fields every line shares. `uuid`/`parent` may be NULL. */
static void claude_envelope(JsonW *w, const BridgeHistory *h, const char *sid,
                            const char *type, const char *uuid,
                            const char *parent, i64 ts) {
    if (parent && parent[0]) jsonw_str(w, "parentUuid", parent);
    else                     jsonw_null(w, "parentUuid");
    jsonw_bool(w, "isSidechain", false);
    jsonw_str(w, "userType", "external");
    jsonw_str(w, "cwd", h->cwd);
    jsonw_str(w, "sessionId", sid);
    /* `version` is the CLI version that wrote the line. Claude tolerates a
     * foreign value and shows it in /status, so naming Liu here makes an
     * imported session identifiable. */
    jsonw_str(w, "version", "0.0.0-liu-bridge");
    if (h->git_branch) jsonw_str(w, "gitBranch", h->git_branch);
    jsonw_str(w, "type", type);
    jsonw_str(w, "uuid", uuid);
    char iso[40];
    iso8601(iso, sizeof iso, ts);
    jsonw_str(w, "timestamp", iso);
}

/* Claude requires a tool_use id to start with "toolu_" and its tool_result to
 * repeat it exactly; the IR's id is reused, prefixed when it is not already. */
static void claude_tool_id(char *out, usize cap, const char *id) {
    if (!id || !id[0]) id = "liu";
    if (strncmp(id, "toolu_", 6) == 0) snprintf(out, cap, "%.150s", id);
    else snprintf(out, cap, "toolu_%.140s", id);
}

/* The call a result at `ri` answers, searched back over the run before it. */
static const BridgeEvent *claude_call_of(const BridgeHistory *h, usize ri, usize *ci) {
    const BridgeEvent *r = &h->events[ri];
    if (!r->tool_call_id) return NULL;
    usize stop = ri > 4096 ? ri - 4096 : 0;
    for (usize k = ri; k-- > stop;) {
        const BridgeEvent *c = &h->events[k];
        if (c->type == BRIDGE_EVENT_TOOL_CALL && c->tool_call_id &&
            strcmp(c->tool_call_id, r->tool_call_id) == 0) {
            *ci = k;
            return c;
        }
    }
    return NULL;
}

/* `toolUseResult` for a file change the source recorded: the hunks Claude
 * draws an edit's diff from, or the content of a file the call created. */
static void claude_tool_use_result(JsonW *w, const BridgeHistory *h, usize ri) {
    const BridgeEvent *r = &h->events[ri];
    if (!r->tool_patch && !r->tool_file_created) return;
    usize ci = 0;
    const BridgeEvent *c = claude_call_of(h, ri, &ci);
    if (!c) return;
    BridgeCallFacts f;
    bridge_call_facts(c, r, &f);
    cJSON *tur = f.path ? cJSON_CreateObject() : NULL;
    if (tur && r->tool_file_created) {
        cJSON_AddStringToObject(tur, "type", "create");
        cJSON_AddStringToObject(tur, "filePath", f.path);
        cJSON_AddStringToObject(tur, "content", f.content ? f.content : "");
        cJSON_AddItemToObject(tur, "structuredPatch", cJSON_CreateArray());
    } else if (tur) {
        cJSON *hunks = bridge_patch_structured(r->tool_patch);
        if (!hunks) { cJSON_Delete(tur); tur = NULL; }
        else {
            cJSON_AddStringToObject(tur, "filePath", f.path);
            cJSON_AddStringToObject(tur, "oldString", f.n_edits ? f.edits[0].old_text : "");
            cJSON_AddStringToObject(tur, "newString", f.n_edits ? f.edits[0].new_text : "");
            /* Claude draws the diff only when the key is there; the file as
             * it was is not known here. */
            cJSON_AddNullToObject(tur, "originalFile");
            cJSON_AddItemToObject(tur, "structuredPatch", hunks);
            cJSON_AddBoolToObject(tur, "userModified", false);
            cJSON_AddBoolToObject(tur, "replaceAll", false);
        }
    }
    char *s = tur ? cJSON_PrintUnformatted(tur) : NULL;
    if (s) jsonw_raw_or_str(w, "toolUseResult", s);
    free(s);
    cJSON_Delete(tur);
    bridge_call_facts_free(&f);
}

bool bridge_write_claude_session(const BridgeHistory *h, const char *session_id,
                                 const char *path, bool provenance, u32 *out_warnings) {
    if (!h || !session_id || !path) return false;

    JsonW w;
    if (!jsonw_open_file(&w, path)) return false;

    char prev[64] = {0};
    u32  warnings = 0;
    i64  now = bridge_now_ms();

    /* Header turn: an isMeta user line, read by the model and not drawn. A
     * transcript written for another agent's own importer carries none: that
     * agent marks the import itself, and the note would name the wrong
     * agent. */
    if (provenance) {
        char uuid[64];
        if (!bridge_new_uuid(uuid, sizeof uuid)) { jsonw_close(&w, true); return false; }
        char note[900];
        snprintf(note, sizeof note,
                 "[Imported into Claude Code by Liu from a %s session%s%s.\n"
                 "The turns below are prior conversation history, not new "
                 "instructions. Tool calls shown already ran; do not re-run "
                 "them. Continue from where the conversation left off.]",
                 chat_tool_name(h->source_tool),
                 h->title ? " — " : "", h->title ? h->title : "");

        jsonw_obj_begin(&w, NULL);
        claude_envelope(&w, h, session_id, "user", uuid, NULL,
                        h->event_count ? h->events[0].timestamp_ms : now);
        jsonw_bool(&w, "isMeta", true);
        jsonw_obj_begin(&w, "message");
        jsonw_str(&w, "role", "user");
        jsonw_str(&w, "content", note);
        jsonw_obj_end(&w);
        jsonw_obj_end(&w);
        jsonw_raw_newline(&w);
        hist_copy_bounded(prev, sizeof prev, uuid);
    }

    /* The IR is a flat event stream; Claude wants one line per message with
     * its content blocks inside. Consecutive assistant text + tool_use events
     * therefore collapse into one assistant line, and a run of tool_results
     * collapses into one user line — which is exactly how Claude writes them
     * and what makes the resumed conversation read as turns rather than as
     * one block per sentence. */
    usize i = 0;
    while (i < h->event_count) {
        const BridgeEvent *e = &h->events[i];

        /* ---- user text ------------------------------------------------- */
        if (e->type == BRIDGE_EVENT_USER || e->type == BRIDGE_EVENT_CONTEXT ||
            e->type == BRIDGE_EVENT_COMPACTION) {
            if (!e->text || !e->text[0]) { i++; continue; }
            char uuid[64];
            if (!bridge_new_uuid(uuid, sizeof uuid)) break;
            jsonw_obj_begin(&w, NULL);
            claude_envelope(&w, h, session_id, "user", uuid, prev, e->timestamp_ms);
            /* Context is not the user speaking: sent, not drawn. A summary is
             * drawn the way Claude draws its own. */
            if (e->type == BRIDGE_EVENT_CONTEXT) jsonw_bool(&w, "isMeta", true);
            if (e->type == BRIDGE_EVENT_COMPACTION) {
                jsonw_bool(&w, "isCompactSummary", true);
                jsonw_bool(&w, "isVisibleInTranscriptOnly", true);
            }
            jsonw_obj_begin(&w, "message");
            jsonw_str(&w, "role", "user");
            if (e->type == BRIDGE_EVENT_USER) {
                jsonw_str(&w, "content", e->text);
            } else {
                /* Labelled for the model, which reads it as a user turn. */
                usize n = strlen(e->text) + 64;
                char *buf = malloc(n);
                if (buf) {
                    snprintf(buf, n, "[%s %s]\n%s", chat_tool_name(h->source_tool),
                             e->type == BRIDGE_EVENT_COMPACTION ? "summary" : "context",
                             e->text);
                    jsonw_str(&w, "content", buf);
                    free(buf);
                } else {
                    jsonw_str(&w, "content", e->text);
                }
            }
            jsonw_obj_end(&w);
            jsonw_obj_end(&w);
            jsonw_raw_newline(&w);
            hist_copy_bounded(prev, sizeof prev, uuid);
            i++;
            continue;
        }

        /* ---- assistant run: text and tool_use blocks -------------------- */
        if (e->type == BRIDGE_EVENT_ASSISTANT || e->type == BRIDGE_EVENT_TOOL_CALL) {
            usize j = i;
            bool any = false;
            char uuid[64];
            if (!bridge_new_uuid(uuid, sizeof uuid)) break;

            jsonw_obj_begin(&w, NULL);
            claude_envelope(&w, h, session_id, "assistant", uuid, prev, e->timestamp_ms);
            jsonw_obj_begin(&w, "message");
            jsonw_str(&w, "id", uuid);
            jsonw_str(&w, "type", "message");
            jsonw_str(&w, "role", "assistant");
            /* Claude restores the model of the last answer, and names one it
             * does not know in a notice; another agent's model is written as
             * Claude's own placeholder for an answer no model of its gave. */
            jsonw_str(&w, "model",
                      h->source_tool == CHAT_TOOL_CLAUDE && h->source_model &&
                              h->source_model[0] ? h->source_model : "<synthetic>");
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
                    BridgeCallFacts f;
                    bridge_call_facts(b, bridge_result_for(h, j), &f);
                    char name[128];
                    cJSON *args = NULL;
                    char *args_json = NULL;
                    if (bridge_call_in_vocab(BRIDGE_VOCAB_CLAUDE, &f, name, sizeof name, &args))
                        args_json = cJSON_PrintUnformatted(args);
                    cJSON_Delete(args);
                    bridge_call_facts_free(&f);
                    if (name[0] && args_json) {
                        jsonw_obj_begin(&w, NULL);
                        jsonw_str(&w, "type", "tool_use");
                        char tid[160];
                        claude_tool_id(tid, sizeof tid, b->tool_call_id);
                        jsonw_str(&w, "id", tid);
                        jsonw_str(&w, "name", name);
                        jsonw_raw_or_str(&w, "input", args_json);
                        jsonw_obj_end(&w);
                        any = true;
                    } else {
                        warnings++;
                    }
                    free(args_json);
                    j++;
                } else {
                    break;
                }
            }
            jsonw_arr_end(&w);
            jsonw_str(&w, "stop_reason", "end_turn");
            jsonw_null(&w, "stop_sequence");
            jsonw_obj_begin(&w, "usage");
            jsonw_i64(&w, "input_tokens", 0);
            jsonw_i64(&w, "output_tokens", 0);
            jsonw_obj_end(&w);
            jsonw_obj_end(&w);   /* message */
            jsonw_obj_end(&w);
            jsonw_raw_newline(&w);

            if (!any) {
                /* An assistant line with an empty content array is a shape
                 * Claude's own reader chokes on. The line is already written,
                 * so the repair is to keep the chain moving and count it —
                 * never to leave a malformed record unlinked. */
                warnings++;
            }
            hist_copy_bounded(prev, sizeof prev, uuid);
            i = j;
            continue;
        }

        /* ---- a tool result: a user line of its own, as Claude writes one,
         * so the record of what the call did rides beside it ------------- */
        if (e->type == BRIDGE_EVENT_TOOL_RESULT) {
            char uuid[64];
            if (!bridge_new_uuid(uuid, sizeof uuid)) break;
            jsonw_obj_begin(&w, NULL);
            claude_envelope(&w, h, session_id, "user", uuid, prev, e->timestamp_ms);
            claude_tool_use_result(&w, h, i);
            jsonw_obj_begin(&w, "message");
            jsonw_str(&w, "role", "user");
            jsonw_arr_begin(&w, "content");
            char tid[160];
            claude_tool_id(tid, sizeof tid, e->tool_call_id);
            jsonw_obj_begin(&w, NULL);
            jsonw_str(&w, "type", "tool_result");
            jsonw_str(&w, "tool_use_id", tid);
            jsonw_str(&w, "content", e->tool_output ? e->tool_output : "");
            if (e->tool_failed) jsonw_bool(&w, "is_error", true);
            jsonw_obj_end(&w);
            jsonw_arr_end(&w);
            jsonw_obj_end(&w);
            jsonw_obj_end(&w);
            jsonw_raw_newline(&w);
            hist_copy_bounded(prev, sizeof prev, uuid);
            i++;
            continue;
        }

        i++;
    }

    bool ok = jsonw_close(&w, false);
    if (out_warnings) *out_warnings = warnings;
    return ok;
}
