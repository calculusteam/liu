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
 * Deliberately NOT written: `thinking` blocks (another model's private
 * channel), `isMeta` lines, `file-history-snapshot` / `file-history-delta`
 * (they reference backup blobs that do not exist for an imported session, and
 * a dangling reference is worse than an absent one), and any of the
 * permission / mode records — an imported session must start under whatever
 * policy the user's Claude is configured with, never one carried across.
 */
#include "history/bridge/writers.h"
#include "history/bridge/jsonw.h"
#include "history/bridge/tool_map.h"
#include "history/util.h"

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

    /* Claude's own rule (hist_slugify_claude): every byte that is not
     * [A-Za-z0-9] becomes '-'. Turning only '/' into '-' would put a
     * session for "/w/My App.v2" in a directory Claude never looks in for
     * that cwd, and `claude --resume` could not find it. */
    char slug[BRIDGE_PATH_CAP];
    char trimmed[BRIDGE_PATH_CAP];
    usize n = strlen(cwd);
    if (n + 1 >= sizeof trimmed) return false;
    memcpy(trimmed, cwd, n + 1);
    /* A trailing slash is the same directory (and would be a trailing dash,
     * splitting one project into two). */
    while (n > 1 && trimmed[n - 1] == '/') trimmed[--n] = '\0';
    if (!hist_slugify_claude(slug, sizeof slug, trimmed)) return false;

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

bool bridge_write_claude_session(const BridgeHistory *h, const char *session_id,
                                 const char *path, u32 *out_warnings) {
    if (!h || !session_id || !path) return false;

    JsonW w;
    if (!jsonw_open_file(&w, path)) return false;

    char prev[64] = {0};
    u32  warnings = 0;
    i64  now = bridge_now_ms();

    /* Header turn. Claude's resume shows the conversation from its first user
     * message, so the provenance note has to BE a user message — there is no
     * system channel in this format that survives a resume. */
    {
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
            jsonw_obj_begin(&w, "message");
            jsonw_str(&w, "role", "user");
            if (e->type == BRIDGE_EVENT_USER) {
                jsonw_str(&w, "content", e->text);
            } else {
                /* Context and compaction are not the user speaking. There is
                 * no channel for that here, so they are labelled inline. */
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
            jsonw_str(&w, "model",
                      h->source_model && h->source_model[0] ? h->source_model
                                                            : "liu-bridge-import");
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
                    const char *name = bridge_tool_name_for(CHAT_TOOL_CLAUDE, b->tool_kind);
                    if (!name) name = b->tool_name;
                    if (name && name[0]) {
                        jsonw_obj_begin(&w, NULL);
                        jsonw_str(&w, "type", "tool_use");
                        /* Claude requires tool_use ids to start with "toolu_"
                         * and match their tool_result's tool_use_id exactly.
                         * The IR's own id is reused with that prefix so the
                         * pairing below cannot drift. */
                        char tid[160];
                        snprintf(tid, sizeof tid, "toolu_%.140s",
                                 b->tool_call_id ? b->tool_call_id : "liu");
                        jsonw_str(&w, "id", tid);
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

        /* ---- tool results: one user line carrying the blocks ------------ */
        if (e->type == BRIDGE_EVENT_TOOL_RESULT) {
            usize j = i;
            char uuid[64];
            if (!bridge_new_uuid(uuid, sizeof uuid)) break;
            jsonw_obj_begin(&w, NULL);
            claude_envelope(&w, h, session_id, "user", uuid, prev, e->timestamp_ms);
            jsonw_obj_begin(&w, "message");
            jsonw_str(&w, "role", "user");
            jsonw_arr_begin(&w, "content");
            while (j < h->event_count && h->events[j].type == BRIDGE_EVENT_TOOL_RESULT) {
                const BridgeEvent *b = &h->events[j];
                char tid[160];
                snprintf(tid, sizeof tid, "toolu_%.140s",
                         b->tool_call_id ? b->tool_call_id : "liu");
                jsonw_obj_begin(&w, NULL);
                jsonw_str(&w, "type", "tool_result");
                jsonw_str(&w, "tool_use_id", tid);
                jsonw_str(&w, "content", b->tool_output ? b->tool_output : "");
                jsonw_obj_end(&w);
                j++;
            }
            jsonw_arr_end(&w);
            jsonw_obj_end(&w);
            jsonw_obj_end(&w);
            jsonw_raw_newline(&w);
            hist_copy_bounded(prev, sizeof prev, uuid);
            i = j;
            continue;
        }

        i++;
    }

    bool ok = jsonw_close(&w, false);
    if (out_warnings) *out_warnings = warnings;
    return ok;
}
