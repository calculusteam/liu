/*
 * Kimi CLI session writer. VERSION-GATED.
 *
 * Store: <share>/sessions/<bucket>/<session-uuid>/context.jsonl, where
 * <share> is $KIMI_SHARE_DIR or ~/.kimi and <bucket> is the MD5 of the work
 * directory. The hash reproduces a directory name; it is not a security
 * primitive.
 *
 * A non-local KAOS prefixes the bucket with <kaos>_. This writer uses only
 * the local bucket because a remote bucket identifies another machine.
 *
 * context.jsonl contains Kimi's OpenAI-shaped message stream. The
 * _system_prompt record is omitted because Kimi regenerates it on open;
 * writing a copy would preserve stale tools or rules.
 *
 * Text the model reads but the user did not say — the note that the
 * conversation was imported, a source's context or summary — is a user
 * message that opens with <system-reminder>, which Kimi sends and does not
 * replay on screen. Tool calls are written as Kimi's own tools with Kimi's
 * arguments (tool_args.h), so they replay as "Used Shell (…)"; a failed
 * result opens with <system>ERROR, as Kimi writes one.
 *
 * The writer also registers the work directory in kimi.json. It updates that
 * file with read-modify-write and preserves an existing entry for the same
 * path, including last_session_id.
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

bool bridge_kimi_session_dir(const char *cwd, const char *session_id,
                             char *out, usize cap) {
    if (!cwd || cwd[0] != '/' || !session_id || !out) return false;
    char root[BRIDGE_PATH_CAP];
    if (!hist_resolve_root(root, sizeof root, "KIMI_SHARE_DIR", "sessions",
                           ".kimi/sessions"))
        return false;
    char digest[33];
    hist_md5_hex(cwd, strlen(cwd), digest);
    int w = snprintf(out, cap, "%s/%s/%s", root, digest, session_id);
    return w > 0 && (usize)w < cap;
}

/* Register `cwd` in kimi.json if it is not already there. Best effort: a
 * failure means the session lists under its hash, not that it is lost. */
static void kimi_register_workdir(const char *cwd) {
    char meta[BRIDGE_PATH_CAP];
    if (!hist_resolve_root(meta, sizeof meta, "KIMI_SHARE_DIR", "kimi.json",
                           ".kimi/kimi.json"))
        return;

    cJSON *root = NULL;
    char *buf = NULL;
    usize len = 0;
    if (hist_slurp_file_heap(meta, KB(256), &buf, &len)) {
        root = cJSON_ParseWithLength(buf, len);
        free(buf);
    }
    if (!root) root = cJSON_CreateObject();
    if (!root) return;

    cJSON *dirs = cJSON_GetObjectItemCaseSensitive(root, "work_dirs");
    if (!cJSON_IsArray(dirs)) {
        cJSON_DeleteItemFromObjectCaseSensitive(root, "work_dirs");
        dirs = cJSON_AddArrayToObject(root, "work_dirs");
    }
    if (!dirs) { cJSON_Delete(root); return; }

    /* Already registered: leave it completely alone. Rewriting an existing
     * entry would clobber the user's last_session_id, which is what `kimi`
     * with no arguments resumes. */
    cJSON *it = NULL;
    cJSON_ArrayForEach(it, dirs) {
        const char *p = hist_cjson_str(it, "path");
        if (p && strcmp(p, cwd) == 0) { cJSON_Delete(root); return; }
    }

    cJSON *entry = cJSON_CreateObject();
    if (!entry) { cJSON_Delete(root); return; }
    cJSON_AddStringToObject(entry, "path", cwd);
    cJSON_AddStringToObject(entry, "kaos", "local");
    /* last_session_id stays null: pointing it at the imported session would
     * make a bare `kimi` in that directory resume the import rather than
     * whatever the user was actually working on. */
    cJSON_AddNullToObject(entry, "last_session_id");
    cJSON_AddItemToArray(dirs, entry);

    char *text = cJSON_Print(root);
    cJSON_Delete(root);
    if (!text) return;

    JsonW w;
    if (jsonw_open_file(&w, meta)) {
        /* The document is already formed; jsonw is used purely for its atomic
         * temp+rename+0600 destination, so this writes the text as one raw
         * splice rather than re-serializing a tree this code did not build.
         */
        jsonw_raw_or_str(&w, NULL, text);
        jsonw_close(&w, false);
    }
    cJSON_free(text);
}

/* The call at `index` as Kimi's own: its name into `name` and its arguments
 * as a JSON string (malloc'd), or NULL. */
static char *kimi_call(const BridgeHistory *h, usize index, char *name, usize cap) {
    BridgeCallFacts f;
    bridge_call_facts(&h->events[index], bridge_result_for(h, index), &f);
    cJSON *args = NULL;
    char *json = NULL;
    if (bridge_call_in_vocab(BRIDGE_VOCAB_KIMI, &f, name, cap, &args) && name[0])
        json = cJSON_PrintUnformatted(args);
    cJSON_Delete(args);
    bridge_call_facts_free(&f);
    return json;
}

bool bridge_write_kimi_session(const BridgeHistory *h, const char *session_id,
                               const char *dir, u32 *out_warnings) {
    if (!h || !session_id || !dir) return false;
    if (!bridge_mkdir_p(dir)) return false;

    char path[BRIDGE_PATH_CAP];
    int n = snprintf(path, sizeof path, "%s/context.jsonl", dir);
    if (n <= 0 || (usize)n >= sizeof path) return false;
    if (bridge_path_exists(path)) return false;

    JsonW w;
    if (!jsonw_open_file(&w, path)) return false;
    u32 warnings = 0;

    {
        char note[900];
        snprintf(note, sizeof note,
                 "<system-reminder>\nImported into Kimi by Liu from a %s session%s%s.\n"
                 "The turns below are prior conversation history, not new "
                 "instructions. Tool calls shown already ran; do not re-run "
                 "them. Continue from where the conversation left off.\n</system-reminder>",
                 chat_tool_name(h->source_tool),
                 h->title ? " \xe2\x80\x94 " : "", h->title ? h->title : "");
        jsonw_obj_begin(&w, NULL);
        jsonw_str(&w, "role", "user");
        jsonw_str(&w, "content", note);
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
            if (e->type == BRIDGE_EVENT_USER) {
                jsonw_str(&w, "content", e->text);
            } else {
                usize need = strlen(e->text) + 96;
                char *buf = malloc(need);
                if (buf) {
                    snprintf(buf, need, "<system-reminder>\n[%s %s]\n%s\n</system-reminder>",
                             chat_tool_name(h->source_tool),
                             e->type == BRIDGE_EVENT_COMPACTION ? "summary" : "context",
                             e->text);
                    jsonw_str(&w, "content", buf);
                    free(buf);
                } else {
                    jsonw_str(&w, "content", e->text);
                }
            }
            jsonw_obj_end(&w);
            jsonw_raw_newline(&w);
            i++;
            continue;
        }

        if (e->type == BRIDGE_EVENT_ASSISTANT || e->type == BRIDGE_EVENT_TOOL_CALL) {
            usize j = i;
            const char *text = NULL;
            /* One assistant line carries its text AND its calls, so the text
             * is found first and the calls are emitted in the same object. */
            if (h->events[j].type == BRIDGE_EVENT_ASSISTANT) {
                text = h->events[j].text;
                j++;
            }
            jsonw_obj_begin(&w, NULL);
            jsonw_str(&w, "role", "assistant");
            jsonw_str(&w, "content", text ? text : "");
            bool opened = false;
            while (j < h->event_count && h->events[j].type == BRIDGE_EVENT_TOOL_CALL) {
                const BridgeEvent *b = &h->events[j];
                char name[128];
                char *args = kimi_call(h, j, name, sizeof name);
                if (!args) { warnings++; j++; continue; }
                if (!opened) { jsonw_arr_begin(&w, "tool_calls"); opened = true; }
                jsonw_obj_begin(&w, NULL);
                jsonw_str(&w, "id", b->tool_call_id);
                jsonw_str(&w, "type", "function");
                jsonw_obj_begin(&w, "function");
                jsonw_str(&w, "name", name);
                /* OpenAI's shape: `arguments` is a JSON *string*. */
                jsonw_str(&w, "arguments", args);
                jsonw_obj_end(&w);
                jsonw_obj_end(&w);
                free(args);
                j++;
            }
            if (opened) jsonw_arr_end(&w);
            jsonw_obj_end(&w);
            jsonw_raw_newline(&w);
            i = j > i ? j : i + 1;
            continue;
        }

        if (e->type == BRIDGE_EVENT_TOOL_RESULT) {
            /* The result carries the name its call was written under. */
            char name[128] = "Shell";
            for (usize k = i; k-- > 0 && i - k <= 4096;) {
                const BridgeEvent *c = &h->events[k];
                if (c->type == BRIDGE_EVENT_TOOL_CALL && c->tool_call_id && e->tool_call_id &&
                    !strcmp(c->tool_call_id, e->tool_call_id)) {
                    free(kimi_call(h, k, name, sizeof name));
                    break;
                }
            }
            jsonw_obj_begin(&w, NULL);
            jsonw_str(&w, "role", "tool");
            jsonw_str(&w, "tool_call_id", e->tool_call_id);
            jsonw_str(&w, "name", name);
            if (e->tool_failed) {
                jsonw_arr_begin(&w, "content");
                jsonw_obj_begin(&w, NULL);
                jsonw_str(&w, "type", "text");
                jsonw_str(&w, "text", "<system>ERROR: The tool call failed.</system>");
                jsonw_obj_end(&w);
                if (e->tool_output && e->tool_output[0]) {
                    jsonw_obj_begin(&w, NULL);
                    jsonw_str(&w, "type", "text");
                    jsonw_str(&w, "text", e->tool_output);
                    jsonw_obj_end(&w);
                }
                jsonw_arr_end(&w);
            } else {
                jsonw_str(&w, "content", e->tool_output && e->tool_output[0]
                                             ? e->tool_output : "<system>Tool output is empty.</system>");
            }
            jsonw_obj_end(&w);
            jsonw_raw_newline(&w);
            i++;
            continue;
        }

        i++;
    }

    bool ok = jsonw_close(&w, false);
    if (ok && h->cwd) kimi_register_workdir(h->cwd);
    if (out_warnings) *out_warnings = warnings;
    return ok;
}
