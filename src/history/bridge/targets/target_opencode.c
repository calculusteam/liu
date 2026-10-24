/*
 * This adapter uses OpenCode's import commands and session document formats;
 * it does not write opencode.db directly. It writes a temporary document under
 * Liu's private state directory with mode 0600, imports it, then removes the
 * plaintext copy.
 *
 * For the 1.x line, the adapter writes the export document consumed by
 * opencode import. The importer requires info.agent; message info includes id,
 * sessionID, role, and creation time; assistant messages also include model,
 * provider, path, cost, tokens, and completion time. Each part includes id,
 * sessionID, and messageID, with IDs shaped as ses_, msg_, or prt_ prefixes.
 *
 * For the 2.x line, the adapter uses opencode session import [--directory
 * <dir>] <file> and the projected session transcript format. The document
 * contains session metadata and user or assistant messages with text and tool
 * content; a tool part stores both the call and result in state. The server
 * derives the project from --directory. The 2.x store uses session_v2 and
 * session_message tables rather than session and message tables.
 */
#include "history/bridge/targets.h"
#include "history/bridge/compatibility.h"
#include "history/bridge/jsonw.h"
#include "history/bridge/tool_map.h"
#include "history/util.h"

#include "sqlite3.h"
#include "core/sqlite_harden.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define OC_IMPORT_TIMEOUT_SEC 120

static bool oc_available(void) { return bridge_binary_available("opencode"); }
static bool oc_full_history(void) { return true; }

/* OpenCode's ids are `<prefix>_<26 base32 chars>`; the importer keys parts to
 * messages by exact string, so they only have to be unique and well-shaped. */
static void oc_id(char *out, usize cap, const char *prefix, u32 seq) {
    char tail[32];
    if (!bridge_new_short_id(tail, sizeof tail)) {
        snprintf(tail, sizeof tail, "liu%023u", seq);
    }
    /* Keep the sequence in the low digits so ids sort in emission order,
     * which is what makes a partially-imported session readable. */
    snprintf(out, cap, "%s_%.*s%04u", prefix, 22, tail, seq % 10000u);
}

typedef struct {
    char id[64];
    bool assistant;
    u32  parts;
} OcMsg;

static void oc_write_tokens(JsonW *w, const char *key) {
    jsonw_obj_begin(w, key);
    jsonw_i64(w, "total", 0);
    jsonw_i64(w, "input", 0);
    jsonw_i64(w, "output", 0);
    jsonw_i64(w, "reasoning", 0);
    jsonw_obj_begin(w, "cache");
    jsonw_i64(w, "read", 0);
    jsonw_i64(w, "write", 0);
    jsonw_obj_end(w);
    jsonw_obj_end(w);
}

static void oc_msg_info(JsonW *w, const BridgeHistory *h, const OcMsg *m,
                        const char *sid, const char *parent, i64 ts) {
    jsonw_obj_begin(w, "info");
    jsonw_str(w, "id", m->id);
    jsonw_str(w, "sessionID", sid);
    jsonw_str(w, "role", m->assistant ? "assistant" : "user");
    if (m->assistant) {
        if (parent && parent[0]) jsonw_str(w, "parentID", parent);
        jsonw_str(w, "mode", "build");
        jsonw_str(w, "agent", "build");
        /* providerID/modelID are recorded, not honoured: OpenCode picks the
         * model for the NEXT turn from the user's own config. Naming the
         * source model here is what lets a reader see where the turns came
         * from without it changing anything. */
        jsonw_str(w, "providerID", "liu-bridge");
        jsonw_str(w, "modelID",
                  h->source_model && h->source_model[0] ? h->source_model
                                                        : chat_tool_name(h->source_tool));
        jsonw_obj_begin(w, "path");
        jsonw_str(w, "cwd",  h->cwd);
        jsonw_str(w, "root", h->cwd);
        jsonw_obj_end(w);
        jsonw_f64(w, "cost", 0.0);
        oc_write_tokens(w, "tokens");
        jsonw_obj_begin(w, "time");
        jsonw_i64(w, "created",   ts);
        jsonw_i64(w, "completed", ts);
        jsonw_obj_end(w);
        jsonw_str(w, "finish", "stop");
    } else {
        jsonw_str(w, "agent", "build");
        /* A USER message needs `model` as an OBJECT and a `summary` — the two
         * fields that look like assistant-only bookkeeping and are not. An
         * import without them fails with `Missing key at ["model"]` and no
         * session is created. The assistant branch above uses the FLAT
         * modelID/providerID spelling instead; the two roles genuinely
         * differ, which is why this is not one shared block. */
        jsonw_obj_begin(w, "model");
        jsonw_str(w, "providerID", "liu-bridge");
        jsonw_str(w, "modelID",
                  h->source_model && h->source_model[0] ? h->source_model
                                                        : chat_tool_name(h->source_tool));
        jsonw_obj_end(w);
        jsonw_obj_begin(w, "summary");
        jsonw_arr_begin(w, "diffs");
        jsonw_arr_end(w);
        jsonw_obj_end(w);
        jsonw_obj_begin(w, "time");
        jsonw_i64(w, "created", ts);
        jsonw_obj_end(w);
    }
    jsonw_obj_end(w);
}

/* Which TOOL_RESULT belongs to which TOOL_CALL, by call id. A call's result is
 * NOT always the next tool event: a turn with parallel calls is
 * call A, call B, result A, result B, and a lookup that stopped at the next
 * call gave A no output and then dropped result A as "already paired". The
 * window bounds the search; a result nothing claims is an orphan and is
 * written on its own. */
#define OC_NONE ((usize)-1)
#define OC_PAIR_WINDOW 4096

typedef struct {
    usize *result_of;   /* per event: the result folded into this call, or OC_NONE */
    u8    *claimed;     /* per event: this result was folded into a call           */
} OcPairs;

static void oc_pairs_free(OcPairs *p) {
    free(p->result_of);
    free(p->claimed);
    p->result_of = NULL;
    p->claimed = NULL;
}

static bool oc_pairs_build(const BridgeHistory *h, OcPairs *p) {
    p->result_of = NULL;
    p->claimed = NULL;
    usize n = h->event_count;
    if (n == 0) return true;
    p->result_of = malloc(n * sizeof *p->result_of);
    p->claimed = calloc(n, 1);
    if (!p->result_of || !p->claimed) { oc_pairs_free(p); return false; }
    for (usize i = 0; i < n; i++) p->result_of[i] = OC_NONE;
    for (usize i = 0; i < n; i++) {
        const BridgeEvent *c = &h->events[i];
        if (c->type != BRIDGE_EVENT_TOOL_CALL || !c->tool_call_id || !c->tool_call_id[0]) continue;
        usize end = (n - i - 1 > OC_PAIR_WINDOW) ? i + 1 + OC_PAIR_WINDOW : n;
        for (usize j = i + 1; j < end; j++) {
            const BridgeEvent *r = &h->events[j];
            if (r->type != BRIDGE_EVENT_TOOL_RESULT || p->claimed[j] || !r->tool_call_id ||
                strcmp(r->tool_call_id, c->tool_call_id) != 0) continue;
            p->result_of[i] = j;
            p->claimed[j] = 1;
            break;
        }
    }
    return true;
}

static const BridgeEvent *oc_pair_result(const BridgeHistory *h, const OcPairs *p, usize call) {
    return p->result_of && p->result_of[call] != OC_NONE ? &h->events[p->result_of[call]] : NULL;
}

/* The document and the command of opencode 1.x. Returns the number of
 * messages written (the provenance note included). */
static u32 oc_write_v1(JsonW *w, const BridgeHistory *h, const char *sid, const OcPairs *pairs) {
    i64 now = bridge_now_ms();
    i64 t0  = now, t1 = now;
    for (usize i = 0; i < h->event_count; i++) {
        i64 t = h->events[i].timestamp_ms;
        if (t <= 0) continue;
        if (t0 == now || t < t0) t0 = t;
        if (t > t1) t1 = t;
    }

    jsonw_obj_begin(w, NULL);

    jsonw_obj_begin(w, "info");
    jsonw_str(w, "id", sid);
    jsonw_str(w, "slug", "liu-bridge");
    jsonw_str(w, "projectID", "liu-bridge");
    jsonw_str(w, "directory", h->cwd);
    jsonw_str(w, "path", "");
    {
        char title[600];
        snprintf(title, sizeof title, "%s (from %s)",
                 h->title && h->title[0] ? h->title : "Continued session",
                 chat_tool_name(h->source_tool));
        jsonw_str(w, "title", title);
    }
    jsonw_str(w, "agent", "build");         /* REQUIRED — see the header */
    jsonw_obj_begin(w, "model");
    jsonw_str(w, "id",
              h->source_model && h->source_model[0] ? h->source_model : "liu-bridge");
    jsonw_str(w, "providerID", "liu-bridge");
    jsonw_obj_end(w);
    jsonw_str(w, "version", "0.0.0");
    jsonw_obj_begin(w, "summary");
    jsonw_i64(w, "additions", 0);
    jsonw_i64(w, "deletions", 0);
    jsonw_i64(w, "files", 0);
    jsonw_obj_end(w);
    jsonw_f64(w, "cost", 0.0);
    jsonw_obj_begin(w, "tokens");
    jsonw_i64(w, "input", 0);
    jsonw_i64(w, "output", 0);
    jsonw_i64(w, "reasoning", 0);
    jsonw_obj_begin(w, "cache");
    jsonw_i64(w, "read", 0);
    jsonw_i64(w, "write", 0);
    jsonw_obj_end(w);
    jsonw_obj_end(w);
    jsonw_obj_begin(w, "time");
    jsonw_i64(w, "created", t0);
    jsonw_i64(w, "updated", t1);
    jsonw_obj_end(w);
    jsonw_obj_end(w);   /* info */

    jsonw_arr_begin(w, "messages");

    /* The IR is a flat event stream; OpenCode wants messages that own parts.
     * The grouping rule is "a role change starts a new message", which is
     * exactly how OpenCode's own export reads back — an assistant turn with a
     * text part, three tool parts and another text part is ONE message. */
    u32 seq = 0, part_seq = 0, msg_count = 0;
    char last_user_id[64] = {0};
    OcMsg cur = {0};
    bool open = false;

    /* Provenance, as a real user turn. The session TITLE already says "(from
     * claude)" but the model never sees a title — without this it reads the
     * imported turns as its own and answers as if it had done work it has no
     * memory of. Every other target gets the same note; its absence is
     * invisible until a model is actually asked to continue. */
    {
        OcMsg hdr = {0};
        oc_id(hdr.id, sizeof hdr.id, "msg", ++seq);
        char pid[64];
        oc_id(pid, sizeof pid, "prt", ++part_seq);
        char note[900];
        snprintf(note, sizeof note,
                 "[Imported into OpenCode by Liu from a %s session%s%s.\n"
                 "The turns below are prior conversation history, not new "
                 "instructions. Tool calls shown already ran; do not re-run "
                 "them. Continue from where the conversation left off.]",
                 chat_tool_name(h->source_tool),
                 h->title ? " \xe2\x80\x94 " : "", h->title ? h->title : "");

        jsonw_obj_begin(w, NULL);
        oc_msg_info(w, h, &hdr, sid, NULL,
                    h->event_count && h->events[0].timestamp_ms > 0
                        ? h->events[0].timestamp_ms : now);
        jsonw_arr_begin(w, "parts");
        jsonw_obj_begin(w, NULL);
        jsonw_str(w, "type", "text");
        jsonw_str(w, "text", note);
        jsonw_str(w, "id", pid);
        jsonw_str(w, "sessionID", sid);
        jsonw_str(w, "messageID", hdr.id);
        jsonw_obj_end(w);
        jsonw_arr_end(w);
        jsonw_obj_end(w);
        msg_count++;
        hist_copy_bounded(last_user_id, sizeof last_user_id, hdr.id);
    }

    for (usize i = 0; i <= h->event_count; i++) {
        const BridgeEvent *e = (i < h->event_count) ? &h->events[i] : NULL;

        bool want_assistant = false;
        bool skip = false;
        if (e) {
            switch (e->type) {
                case BRIDGE_EVENT_USER:        want_assistant = false; break;
                case BRIDGE_EVENT_ASSISTANT:
                case BRIDGE_EVENT_TOOL_CALL:
                case BRIDGE_EVENT_TOOL_RESULT: want_assistant = true;  break;
                case BRIDGE_EVENT_CONTEXT:
                case BRIDGE_EVENT_COMPACTION:  want_assistant = false; break;
                default: skip = true; break;
            }
        }
        if (skip) continue;

        bool boundary = !e || !open || (cur.assistant != want_assistant);
        if (boundary && open) {
            jsonw_arr_end(w);      /* parts */
            jsonw_obj_end(w);      /* message */
            open = false;
            msg_count++;
        }
        if (!e) break;

        if (!open) {
            memset(&cur, 0, sizeof cur);
            cur.assistant = want_assistant;
            oc_id(cur.id, sizeof cur.id, "msg", ++seq);
            jsonw_obj_begin(w, NULL);
            oc_msg_info(w, h, &cur, sid,
                        want_assistant ? last_user_id : NULL,
                        e->timestamp_ms > 0 ? e->timestamp_ms : now);
            if (!want_assistant) hist_copy_bounded(last_user_id, sizeof last_user_id, cur.id);
            jsonw_arr_begin(w, "parts");
            open = true;
        }

        char pid[64];
        oc_id(pid, sizeof pid, "prt", ++part_seq);

        switch (e->type) {
            case BRIDGE_EVENT_USER:
            case BRIDGE_EVENT_ASSISTANT:
                if (!e->text || !e->text[0]) break;
                jsonw_obj_begin(w, NULL);
                jsonw_str(w, "type", "text");
                jsonw_str(w, "text", e->text);
                jsonw_str(w, "id", pid);
                jsonw_str(w, "sessionID", sid);
                jsonw_str(w, "messageID", cur.id);
                jsonw_obj_end(w);
                break;

            case BRIDGE_EVENT_CONTEXT:
            case BRIDGE_EVENT_COMPACTION: {
                if (!e->text || !e->text[0]) break;
                /* Context is real conversation material but is not something
                 * the user typed, so it is labelled in the text itself —
                 * OpenCode's part schema has no channel for it. */
                jsonw_obj_begin(w, NULL);
                jsonw_str(w, "type", "text");
                {
                    usize n = strlen(e->text) + 64;
                    char *buf = malloc(n);
                    if (buf) {
                        snprintf(buf, n, "[%s context]\n%s",
                                 chat_tool_name(h->source_tool), e->text);
                        jsonw_str(w, "text", buf);
                        free(buf);
                    } else {
                        jsonw_str(w, "text", e->text);
                    }
                }
                jsonw_str(w, "id", pid);
                jsonw_str(w, "sessionID", sid);
                jsonw_str(w, "messageID", cur.id);
                jsonw_obj_end(w);
                break;
            }

            case BRIDGE_EVENT_TOOL_CALL: {
                /* One `tool` part carries BOTH halves in OpenCode, so the
                 * call is written here and the matching result is folded in
                 * by looking ahead — writing them as two parts would show a
                 * call that never returned. */
                const char *name = bridge_tool_name_for(CHAT_TOOL_OPENCODE, e->tool_kind);
                if (!name) name = e->tool_name;
                if (!name || !name[0]) name = "bash";

                const BridgeEvent *res = oc_pair_result(h, pairs, i);

                jsonw_obj_begin(w, NULL);
                jsonw_str(w, "type", "tool");
                jsonw_str(w, "tool", name);
                jsonw_str(w, "callID", e->tool_call_id);
                jsonw_obj_begin(w, "state");
                jsonw_str(w, "status", res ? "completed" : "error");
                /* REQUIRED to be an object by opencode's importer. */
                jsonw_obj_or_wrap(w, "input", e->tool_input);
                if (res) {
                    jsonw_str(w, "output", res->tool_output ? res->tool_output : "");
                } else {
                    jsonw_str(w, "output", "");
                    jsonw_str(w, "error",
                              "No output recorded in the source session.");
                }
                {
                    char t[256];
                    snprintf(t, sizeof t, "%s (imported)", name);
                    jsonw_str(w, "title", t);
                }
                jsonw_obj_begin(w, "metadata");
                jsonw_obj_end(w);
                jsonw_obj_begin(w, "time");
                jsonw_i64(w, "start", e->timestamp_ms > 0 ? e->timestamp_ms : now);
                jsonw_i64(w, "end",   e->timestamp_ms > 0 ? e->timestamp_ms : now);
                jsonw_obj_end(w);
                jsonw_obj_end(w);   /* state */
                jsonw_str(w, "id", pid);
                jsonw_str(w, "sessionID", sid);
                jsonw_str(w, "messageID", cur.id);
                jsonw_obj_end(w);
                break;
            }

            case BRIDGE_EVENT_TOOL_RESULT: {
                /* Already folded into its call above. An ORPHAN result — no
                 * preceding call in the source — still carries real context,
                 * so it is written as a completed call with an empty input
                 * rather than dropped. */
                if (pairs->claimed && pairs->claimed[i]) break;
                const char *name = bridge_tool_name_for(CHAT_TOOL_OPENCODE, e->tool_kind);
                if (!name) name = e->tool_name;
                if (!name || !name[0]) name = "bash";
                jsonw_obj_begin(w, NULL);
                jsonw_str(w, "type", "tool");
                jsonw_str(w, "tool", name);
                jsonw_str(w, "callID", e->tool_call_id);
                jsonw_obj_begin(w, "state");
                jsonw_str(w, "status", "completed");
                jsonw_obj_begin(w, "input");
                jsonw_obj_end(w);
                jsonw_str(w, "output", e->tool_output ? e->tool_output : "");
                jsonw_str(w, "title", "imported tool output");
                jsonw_obj_begin(w, "metadata");
                jsonw_obj_end(w);
                jsonw_obj_begin(w, "time");
                jsonw_i64(w, "start", e->timestamp_ms > 0 ? e->timestamp_ms : now);
                jsonw_i64(w, "end",   e->timestamp_ms > 0 ? e->timestamp_ms : now);
                jsonw_obj_end(w);
                jsonw_obj_end(w);
                jsonw_str(w, "id", pid);
                jsonw_str(w, "sessionID", sid);
                jsonw_str(w, "messageID", cur.id);
                jsonw_obj_end(w);
                break;
            }

            default: break;
        }
    }

    jsonw_arr_end(w);   /* messages */
    jsonw_obj_end(w);
    return msg_count;
}

/* ------------------------------------------------------- opencode 2.x --- */

/* The first release line with the v2 CLI and document (see the header). */
#define OC_V2_FIRST "2.0.0"

/* If the installed version cannot be read, use the 2.x CLI and document
 * format. When available, ver receives the detected version. */
static bool oc_installed_is_v2(char *ver, usize cap) {
    ver[0] = '\0';
    if (!bridge_compat_installed_version(CHAT_TOOL_OPENCODE, ver, cap)) return true;
    return bridge_version_cmp(ver, OC_V2_FIRST) >= 0;
}

typedef enum { OC_SIDE_NONE, OC_SIDE_USER, OC_SIDE_ASSISTANT } OcSide;

/* Which kind of message an event becomes. One predicate for the writer and for
 * the question "does the session end on an assistant turn", so they agree. */
static OcSide oc_side(const BridgeEvent *e) {
    switch (e->type) {
        case BRIDGE_EVENT_USER:
        case BRIDGE_EVENT_CONTEXT:
        case BRIDGE_EVENT_COMPACTION:
            return e->text && e->text[0] ? OC_SIDE_USER : OC_SIDE_NONE;
        case BRIDGE_EVENT_ASSISTANT:
            return e->text && e->text[0] ? OC_SIDE_ASSISTANT : OC_SIDE_NONE;
        case BRIDGE_EVENT_TOOL_CALL:
        case BRIDGE_EVENT_TOOL_RESULT:
            return OC_SIDE_ASSISTANT;
        default:
            return OC_SIDE_NONE;
    }
}

static bool oc_ends_with_assistant(const BridgeHistory *h) {
    for (usize i = h->event_count; i-- > 0;) {
        OcSide s = oc_side(&h->events[i]);
        if (s != OC_SIDE_NONE) return s == OC_SIDE_ASSISTANT;
    }
    return false;
}

/* The names a 2.0.32 store uses: `shell` (1.x said `bash`) and `subagent`
 * (`task`); read / grep / glob / edit / write / webfetch / websearch kept
 * theirs. A todo tool was not among them, so that kind keeps the source's
 * own spelling instead of a guess. */
static const char *oc2_tool_name(const BridgeEvent *e) {
    if (e->tool_kind == BRIDGE_TOOL_SHELL)    return "shell";
    if (e->tool_kind == BRIDGE_TOOL_SUBAGENT) return "subagent";
    const char *n = e->tool_kind == BRIDGE_TOOL_TASK
                        ? NULL : bridge_tool_name_for(CHAT_TOOL_OPENCODE, e->tool_kind);
    if (!n) n = e->tool_name;
    return n && n[0] ? n : "shell";
}

typedef struct {
    JsonW               *w;
    const BridgeHistory *h;
    const OcPairs       *pairs;
    i64                  now;
    u32                  msg_seq, call_seq;
    u32                  written;     /* messages from the source (the provenance note is not one) */
    bool                 open;        /* an assistant message is being written ... */
    bool                 has_tool;    /* ... and it holds a tool part */
    i64                  open_t0, open_t1;
} OcV2;

static i64 oc2_ts(const OcV2 *c, i64 ts) { return ts > 0 ? ts : c->now; }

static void oc2_assistant_open(OcV2 *c, i64 ts) {
    if (c->open) return;
    JsonW *w = c->w;
    char id[64];
    oc_id(id, sizeof id, "msg", ++c->msg_seq);
    jsonw_obj_begin(w, NULL);
    jsonw_str(w, "id", id);
    jsonw_str(w, "type", "assistant");
    jsonw_str(w, "agent", "build");
    /* Recorded, not honoured — see oc_msg_info. 2.x wants the pair on every
     * assistant message; the session itself carries no model at all. */
    jsonw_obj_begin(w, "model");
    jsonw_str(w, "id", c->h->source_model && c->h->source_model[0]
                           ? c->h->source_model : chat_tool_name(c->h->source_tool));
    jsonw_str(w, "providerID", "liu-bridge");
    jsonw_obj_end(w);
    jsonw_arr_begin(w, "content");
    c->open = true;
    c->has_tool = false;
    c->open_t0 = c->open_t1 = ts;
}

static void oc2_assistant_close(OcV2 *c) {
    if (!c->open) return;
    JsonW *w = c->w;
    jsonw_arr_end(w);   /* content */
    jsonw_obj_begin(w, "time");
    jsonw_i64(w, "created", c->open_t0);
    jsonw_i64(w, "completed", c->open_t1);
    jsonw_obj_end(w);
    jsonw_str(w, "finish", c->has_tool ? "tool-calls" : "stop");
    jsonw_obj_end(w);
    c->open = false;
    c->written++;
}

static void oc2_user(OcV2 *c, const char *text, i64 ts, bool counted) {
    oc2_assistant_close(c);
    JsonW *w = c->w;
    char id[64];
    oc_id(id, sizeof id, "msg", ++c->msg_seq);
    jsonw_obj_begin(w, NULL);
    jsonw_str(w, "id", id);
    jsonw_str(w, "type", "user");
    jsonw_obj_begin(w, "time");
    jsonw_i64(w, "created", ts);
    jsonw_obj_end(w);
    jsonw_str(w, "text", text);
    jsonw_obj_end(w);
    if (counted) c->written++;
}

static void oc2_text(OcV2 *c, const char *text, i64 ts) {
    /* A step ends at its tool calls: what the model said after them is the
     * next step, and a real store never has it in the same message. */
    if (c->open && c->has_tool) oc2_assistant_close(c);
    oc2_assistant_open(c, ts);
    jsonw_obj_begin(c->w, NULL);
    jsonw_str(c->w, "type", "text");
    jsonw_str(c->w, "text", text);
    jsonw_obj_end(c->w);
    if (ts > c->open_t1) c->open_t1 = ts;
}

/* A tool part holds the call and its result. With no result it is an error
 * part, the way a call the agent never saw finish is stored. */
static void oc2_tool(OcV2 *c, const char *name, const char *call_id, const char *input,
                     bool has_result, const char *output, i64 t_call, i64 t_done) {
    oc2_assistant_open(c, t_call);
    JsonW *w = c->w;
    char fallback[64];
    if (!call_id || !call_id[0]) {
        oc_id(fallback, sizeof fallback, "call", ++c->call_seq);
        call_id = fallback;
    }
    jsonw_obj_begin(w, NULL);
    jsonw_str(w, "type", "tool");
    jsonw_str(w, "id", call_id);
    jsonw_str(w, "name", name);
    jsonw_bool(w, "executed", false);
    jsonw_obj_begin(w, "state");
    jsonw_str(w, "status", has_result ? "completed" : "error");
    /* REQUIRED to be an object, as in 1.x. */
    jsonw_obj_or_wrap(w, "input", input);
    if (has_result) {
        jsonw_arr_begin(w, "content");
        jsonw_obj_begin(w, NULL);
        jsonw_str(w, "type", "text");
        jsonw_str(w, "text", output && output[0] ? output : "(no output)");
        jsonw_obj_end(w);
        jsonw_arr_end(w);
        jsonw_obj_begin(w, "metadata");
        jsonw_obj_end(w);
    } else {
        jsonw_obj_begin(w, "error");
        jsonw_str(w, "type", "tool.execution");
        jsonw_str(w, "message", "No output recorded in the source session.");
        jsonw_obj_end(w);
    }
    jsonw_obj_end(w);   /* state */
    jsonw_obj_begin(w, "time");
    jsonw_i64(w, "created", t_call);
    jsonw_i64(w, "ran", t_call);
    jsonw_i64(w, "completed", t_done);
    jsonw_obj_end(w);
    jsonw_obj_end(w);   /* part */
    c->has_tool = true;
    if (t_done > c->open_t1) c->open_t1 = t_done;
}

/* The opencode 2.x document. Returns the number of messages that came from the
 * source session; 0 means there was nothing worth importing. */
static u32 oc_write_v2(JsonW *w, const BridgeHistory *h, const char *sid, const OcPairs *pairs) {
    OcV2 c = { .w = w, .h = h, .pairs = pairs, .now = bridge_now_ms() };
    i64 t0 = c.now, t1 = c.now;
    for (usize i = 0; i < h->event_count; i++) {
        i64 t = h->events[i].timestamp_ms;
        if (t <= 0) continue;
        if (t0 == c.now || t < t0) t0 = t;
        if (t > t1) t1 = t;
    }
    /* A session that ends on an assistant turn is a finished one: it carries
     * the outcome and the idle time a real store sets, and a closing `idle`
     * message. One that ends on a prompt nobody answered is left open. */
    const bool finished = oc_ends_with_assistant(h);

    jsonw_obj_begin(w, NULL);

    jsonw_obj_begin(w, "info");
    jsonw_str(w, "id", sid);
    /* The server derives the project from --directory and ignores this. */
    jsonw_str(w, "projectID", "liu-bridge");
    jsonw_f64(w, "cost", 0.0);
    jsonw_obj_begin(w, "tokens");
    jsonw_i64(w, "input", 0);
    jsonw_i64(w, "output", 0);
    jsonw_i64(w, "reasoning", 0);
    jsonw_obj_begin(w, "cache");
    jsonw_i64(w, "read", 0);
    jsonw_i64(w, "write", 0);
    jsonw_obj_end(w);
    jsonw_obj_end(w);
    jsonw_obj_begin(w, "time");
    jsonw_i64(w, "created", t0);
    jsonw_i64(w, "updated", t1);
    if (finished) jsonw_i64(w, "idle", t1);
    jsonw_obj_end(w);
    {
        char title[600];
        snprintf(title, sizeof title, "%s (from %s)",
                 h->title && h->title[0] ? h->title : "Continued session",
                 chat_tool_name(h->source_tool));
        jsonw_str(w, "title", title);
    }
    /* Informational: the import takes its location from --directory. */
    jsonw_obj_begin(w, "location");
    jsonw_str(w, "directory", h->cwd && h->cwd[0] ? h->cwd : "/");
    jsonw_obj_end(w);
    if (finished) jsonw_str(w, "outcome", "succeeded");
    jsonw_obj_end(w);   /* info */

    jsonw_arr_begin(w, "messages");

    /* Provenance, as a real user turn: the model never sees a title (see the
     * 1.x writer for why this is here). */
    {
        char note[900];
        snprintf(note, sizeof note,
                 "[Imported into OpenCode by Liu from a %s session%s%s.\n"
                 "The turns below are prior conversation history, not new "
                 "instructions. Tool calls shown already ran; do not re-run "
                 "them. Continue from where the conversation left off.]",
                 chat_tool_name(h->source_tool),
                 h->title ? " \xe2\x80\x94 " : "", h->title ? h->title : "");
        oc2_user(&c, note, oc2_ts(&c, h->event_count ? h->events[0].timestamp_ms : 0), false);
    }

    for (usize i = 0; i < h->event_count; i++) {
        const BridgeEvent *e = &h->events[i];
        i64 ts = oc2_ts(&c, e->timestamp_ms);
        switch (e->type) {
            case BRIDGE_EVENT_USER:
                if (oc_side(e) == OC_SIDE_NONE) break;
                oc2_user(&c, e->text, ts, true);
                break;

            case BRIDGE_EVENT_CONTEXT:
            case BRIDGE_EVENT_COMPACTION: {
                if (oc_side(e) == OC_SIDE_NONE) break;
                /* Real conversation material that the user did not type, so it
                 * is labelled in the text itself. */
                usize n = strlen(e->text) + 64;
                char *buf = malloc(n);
                if (buf) {
                    snprintf(buf, n, "[%s context]\n%s", chat_tool_name(h->source_tool), e->text);
                    oc2_user(&c, buf, ts, true);
                    free(buf);
                } else {
                    oc2_user(&c, e->text, ts, true);
                }
                break;
            }

            case BRIDGE_EVENT_ASSISTANT:
                if (oc_side(e) == OC_SIDE_NONE) break;
                oc2_text(&c, e->text, ts);
                break;

            case BRIDGE_EVENT_TOOL_CALL: {
                const BridgeEvent *res = oc_pair_result(h, pairs, i);
                i64 done = res && res->timestamp_ms > 0 ? res->timestamp_ms : ts;
                oc2_tool(&c, oc2_tool_name(e), e->tool_call_id, e->tool_input,
                         res != NULL, res ? res->tool_output : NULL, ts, done);
                break;
            }

            case BRIDGE_EVENT_TOOL_RESULT:
                /* Folded into its call. One no call claimed still carries real
                 * context: a completed call with no recorded input. */
                if (pairs->claimed && pairs->claimed[i]) break;
                oc2_tool(&c, oc2_tool_name(e), e->tool_call_id, NULL, true, e->tool_output, ts, ts);
                break;

            default:
                break;
        }
    }
    oc2_assistant_close(&c);

    if (finished && c.written > 0) {
        char id[64];
        oc_id(id, sizeof id, "msg", ++c.msg_seq);
        jsonw_obj_begin(w, NULL);
        jsonw_str(w, "id", id);
        jsonw_str(w, "type", "idle");
        jsonw_obj_begin(w, "time");
        jsonw_i64(w, "created", t1);
        jsonw_obj_end(w);
        jsonw_str(w, "outcome", "succeeded");
        jsonw_obj_end(w);
    }

    jsonw_arr_end(w);   /* messages */
    jsonw_obj_end(w);
    return c.written;
}

/* Up one level: "/a/b" -> "/a" -> "/". With `to_root` straight to "/". False
 * once there is nowhere higher to go. */
static bool oc_dir_up(char *where, bool to_root) {
    if (strcmp(where, "/") == 0) return false;
    char *slash = strrchr(where, '/');
    if (to_root || !slash || slash == where) {
        where[0] = '/';
        where[1] = '\0';
        return true;
    }
    *slash = '\0';
    return true;
}

/* Where the session is to be imported: 2.x refuses a --directory that is not
 * there (LocationNotFoundError) and takes the project from it, so a session
 * whose folder was removed lands in the nearest one that is. The folder is
 * never looked at from this thread — a path on a volume that went away blocks
 * a stat for good — the CLI is the one that tries it, and a second answer from
 * it is the way up. */
#define OC_DIR_ATTEMPTS 8

static i32 oc_run_import_v2(const char *cwd, const char *doc, char *out, usize cap) {
    char where[BRIDGE_PATH_CAP];
    where[0] = '\0';
    if (cwd && cwd[0] == '/' && strlen(cwd) < sizeof where) snprintf(where, sizeof where, "%s", cwd);
    i32 rc = -1;
    for (i32 attempt = 0; attempt < OC_DIR_ATTEMPTS; attempt++) {
        const char *argv[7];
        i32 n = 0;
        argv[n++] = "opencode";
        argv[n++] = "session";
        argv[n++] = "import";
        if (where[0]) {
            argv[n++] = "--directory";
            argv[n++] = where;
        }
        argv[n++] = doc;
        argv[n] = NULL;
        rc = bridge_run_capture(argv, out, cap, OC_IMPORT_TIMEOUT_SEC);
        if (rc == 0 || !where[0] || !strstr(out, "LocationNotFoundError")) break;
        if (!oc_dir_up(where, attempt + 2 >= OC_DIR_ATTEMPTS)) break;
    }
    return rc;
}

static bool oc_import(const BridgeHistory *h, BridgeImportResult *r) {
    r->mode = BRIDGE_IMPORT_NATIVE_FILE;

    char dir[BRIDGE_PATH_CAP];
    if (!bridge_state_dir(dir, sizeof dir)) {
        snprintf(r->error, sizeof r->error, "Cannot create Liu's bridge state directory.");
        return false;
    }

    char ver[64];
    const bool v2 = oc_installed_is_v2(ver, sizeof ver);

    char sid[64];
    oc_id(sid, sizeof sid, "ses", 0);

    char doc[BRIDGE_PATH_CAP];
    int dn = snprintf(doc, sizeof doc, "%s/oc-import-%s.json", dir, sid + 4);
    if (dn <= 0 || (usize)dn >= sizeof doc) return false;

    OcPairs pairs;
    if (!oc_pairs_build(h, &pairs)) {
        snprintf(r->error, sizeof r->error, "Out of memory preparing the OpenCode import.");
        return false;
    }

    JsonW w;
    if (!jsonw_open_file(&w, doc)) {
        oc_pairs_free(&pairs);
        snprintf(r->error, sizeof r->error, "Cannot write the OpenCode import document.");
        return false;
    }

    u32 msg_count = v2 ? oc_write_v2(&w, h, sid, &pairs) : oc_write_v1(&w, h, sid, &pairs);
    oc_pairs_free(&pairs);

    if (!jsonw_close(&w, false)) {
        unlink(doc);
        snprintf(r->error, sizeof r->error, "Failed writing the OpenCode import document.");
        return false;
    }
    if (msg_count == 0) {
        unlink(doc);
        snprintf(r->error, sizeof r->error, "Nothing to import.");
        return false;
    }

    char out[4096];
    i32 rc;
    if (v2) {
        rc = oc_run_import_v2(h->cwd, doc, out, sizeof out);
    } else {
        const char *argv[] = { "opencode", "import", doc, NULL };
        rc = bridge_run_capture(argv, out, sizeof out, OC_IMPORT_TIMEOUT_SEC);
    }

    /* The document is a plaintext copy of the whole conversation; it exists
     * only for the length of the import. */
    unlink(doc);

    /* 2.x answers a session it already holds with exit 0 and "Session already
     * exists", so success is the line it prints, not the status. */
    if (rc != 0 || (v2 && !strstr(out, "Imported session"))) {
        if (!strncmp(out, "DESCRIPTION", 11)) {
            /* The CLI did not know the command and printed its whole usage
             * text: a screenful of help is no answer for a toast. */
            snprintf(r->error, sizeof r->error,
                     "OpenCode %s does not accept `opencode %simport`.",
                     ver[0] ? ver : "(unknown version)", v2 ? "session " : "");
        } else {
            /* Pass OpenCode's schema error through verbatim because it
             * identifies the missing field. */
            const char *msg = out[0] ? out : "opencode import failed.";
            snprintf(r->error, sizeof r->error, "OpenCode rejected the import: %.400s", msg);
        }
        return false;
    }

    hist_copy_bounded(r->session_id, sizeof r->session_id, sid);
    snprintf(r->launch_cmd, sizeof r->launch_cmd, "opencode --session %s", sid);
    return true;
}

static bool oc_session_has_messages(sqlite3 *db, const char *sql, const char *id) {
    bool ok = false;
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_text(s, 1, id, -1, SQLITE_STATIC);
        if (sqlite3_step(s) == SQLITE_ROW) ok = sqlite3_column_int64(s, 0) > 0;
        sqlite3_finalize(s);
    }
    return ok;
}

static bool oc_validate(const BridgeImportResult *r) {
    /* A successful exit from opencode import does not guarantee the session
     * is committed. Validate the session using a read-only connection that
     * can read the WAL; an immutable connection would miss the new row. */
    if (!r->session_id[0]) return false;

    char db_path[BRIDGE_PATH_CAP];
    if (!hist_xdg_data_path(db_path, sizeof db_path, "opencode/opencode.db"))
        return false;
    struct stat st;
    if (stat(db_path, &st) != 0 || !S_ISREG(st.st_mode)) return false;

    static bool inited = false;
    if (!inited) { sqlite3_initialize(); inited = true; }

    sqlite3 *db = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return false;
    }
    liu_sqlite_harden_foreign(db);               /* somebody else's file */
    /* 2.x keeps sessions in session_v2 / session_message and has no `session`
     * or `message` table at all; 1.x is the other way round. A statement on a
     * table the store does not have fails to prepare, so each is just asked. */
    bool ok = oc_session_has_messages(db,
                  "SELECT (SELECT COUNT(*) FROM session_message m WHERE m.session_id = s.id) "
                  "FROM session_v2 s WHERE s.id = ?1", r->session_id) ||
              oc_session_has_messages(db,
                  "SELECT (SELECT COUNT(*) FROM message m WHERE m.session_id = s.id) "
                  "FROM session s WHERE s.id = ?1", r->session_id);
    sqlite3_close(db);
    return ok;
}

const BridgeTarget g_bridge_target_opencode = {
    .tool                  = CHAT_TOOL_OPENCODE,
    .mode                  = BRIDGE_IMPORT_NATIVE_FILE,
    .available             = oc_available,
    .supports_full_history = oc_full_history,
    .import_history        = oc_import,
    .validate_import       = oc_validate,
};
