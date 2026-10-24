/*
 * liu-history/bridge - OpenCode source parser.
 *
 * OpenCode stores session data in a SQLite database at
 * $XDG_DATA_HOME/opencode/opencode.db (usually
 * ~/.local/share/opencode/opencode.db). ChatSessionMeta.path for an OpenCode
 * session points to this database.
 *
 * The 1.x schema contains:
 *   session(id, project_id, slug, directory, title, version, agent, model,
 *   time_created, time_updated, ...)
 *   message(id, session_id, time_created, time_updated, data TEXT)  -- JSON
 *   part(id, message_id, session_id, time_created, data TEXT)       -- JSON
 *
 * A message's data contains role, modelID, providerID and time.created. A part
 * may contain text, reasoning, turn scaffolding, or a tool call with its result
 * in the same row. The parser emits separate IR events for the call and result
 * so every target receives the normalized pair.
 *
 * OpenCode 2.x uses the separate schema in history/opencode_store.h:
 *   session_v2(id, parent_id, directory, title, agent, model JSON, ...)
 *   session_message(id, session_id, type, seq, time_created, data JSON)
 * The parser reads user, assistant and completed compaction records. User rows
 * contain text, files and agents; assistant rows contain content parts;
 * completed compaction rows contain a summary. Idle, model-switched,
 * location-switched, synthetic and system records are scaffolding or notices
 * and are skipped.
 *
 * The parser reads only the selected session, because the caller already knows
 * its ID. It also avoids SQLite LENGTH() on TEXT columns: a large value can
 * make SQLite materialize overflow pages just to compute the length. Event
 * bodies are read only for the selected session and remain subject to the IR's
 * per-event caps.
 */
#include "history/bridge/sources.h"
#include "history/bridge/tool_map.h"
#include "history/util.h"
#include "history/opencode_store.h"

#include "cJSON.h"
#include "sqlite3.h"
#include "core/sqlite_harden.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static void oc_sqlite_init(void) {
    /* The vendored build sets SQLITE_OMIT_AUTOINIT, so this is required.
     * Idempotent and cheap on repeat calls. */
    static bool inited = false;
    if (!inited) { sqlite3_initialize(); inited = true; }
}

/* One `part` row -> 0..2 IR events. */
static void oc_part(BridgeHistory *h, Arena *scratch, const char *data,
                    i64 ts_ms, bool assistant) {
    if (!data || !data[0]) return;
    cJSON *p = cJSON_Parse(data);
    if (!p) return;

    const char *type = hist_cjson_str(p, "type");
    if (!type) { cJSON_Delete(p); return; }

    if (strcmp(type, "text") == 0) {
        bridge_push_message(h, assistant ? BRIDGE_EVENT_ASSISTANT : BRIDGE_EVENT_USER,
                            ts_ms, hist_cjson_str(p, "text"));

    } else if (strcmp(type, "tool") == 0) {
        const char *name   = hist_cjson_str(p, "tool");
        const char *callid = hist_cjson_str(p, "callID");
        cJSON *state = cJSON_GetObjectItemCaseSensitive(p, "state");
        const char *status = state ? hist_cjson_str(state, "status") : NULL;

        BridgeToolKind kind = bridge_tool_kind_for(CHAT_TOOL_OPENCODE, name);

        cJSON *input = state ? cJSON_GetObjectItemCaseSensitive(state, "input") : NULL;
        BridgeEvent call = {0};
        call.type         = BRIDGE_EVENT_TOOL_CALL;
        call.timestamp_ms = ts_ms;
        call.tool_name    = name;
        call.tool_call_id = callid;
        call.tool_kind    = kind;
        call.tool_input   = bridge_json_compact(scratch, input);
        bridge_history_push(h, &call);

        /* A pending/running/errored call has no output yet. Emitting an empty
         * result would tell the next agent the tool returned nothing, which
         * is a different fact from "it never finished" — so an error carries
         * its error text and an unfinished call gets no result at all. */
        const char *out = state ? hist_cjson_str(state, "output") : NULL;
        if (!out && state && status && strcmp(status, "error") == 0)
            out = hist_cjson_str(state, "error");
        if (out && out[0]) {
            BridgeEvent res = {0};
            res.type         = BRIDGE_EVENT_TOOL_RESULT;
            res.timestamp_ms = ts_ms;
            res.tool_name    = name;
            res.tool_call_id = callid;
            res.tool_kind    = kind;
            res.tool_output  = out;
            bridge_history_push(h, &res);
        }
    }
    /* reasoning / step-start / step-finish / snapshot / patch: not
     * conversation. `patch` in particular is OpenCode's own file-snapshot
     * bookkeeping, not a tool the model called. */
    cJSON_Delete(p);
}

/* ---------------------------------------------------------------- 2.x --- */

/* Media types go into a message: letters, digits and the few marks one has. */
static bool oc2_mime_ok(const char *m) {
    if (!m || !m[0] || strlen(m) >= 64) return false;
    return strspn(m, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789/+.-") == strlen(m);
}

/* What the user attached cannot be handed on: the turn keeps its place as a
 * note that says what it was, and the loss is counted (see sources.c). */
static void oc2_files(BridgeHistory *h, cJSON *files, i64 ts_ms) {
    cJSON *f;
    cJSON_ArrayForEach(f, files) {
        const char *mime = hist_cjson_str(f, "mime");
        const char *data = hist_cjson_str(f, "data");
        const char *what = mime && strncmp(mime, "image/", 6) == 0 ? "image" : "document";
        char note[160];
        if (data && oc2_mime_ok(mime)) {
            usize n = strlen(data), pad = 0;
            while (pad < 2 && pad < n && data[n - 1 - pad] == '=') pad++;
            usize bytes = n / 4 * 3 + (n % 4 ? n % 4 - 1 : 0);
            snprintf(note, sizeof note, "[%s: %s, %zu bytes]", what, mime, bytes > pad ? bytes - pad : 0);
        } else {
            snprintf(note, sizeof note, "[%s]", what);
        }
        bridge_push_message(h, BRIDGE_EVENT_USER, ts_ms, note);
        h->fidelity.events_dropped++;
        h->fidelity.history_complete = false;
    }
}

/* A tool part's output: its text parts in order; any other kind a result
 * carries is named, not copied. NULL when it has none. */
static const char *oc2_result(Arena *scratch, cJSON *state) {
    const char *status = hist_cjson_str(state, "status");
    if (status && strcmp(status, "error") == 0) {
        cJSON *err = cJSON_GetObjectItemCaseSensitive(state, "error");
        const char *msg = err ? hist_cjson_str(err, "message") : NULL;
        return msg && msg[0] ? msg : NULL;
    }
    cJSON *content = cJSON_GetObjectItemCaseSensitive(state, "content");
    if (!cJSON_IsArray(content)) return NULL;
    usize total = 1;
    cJSON *c;
    cJSON_ArrayForEach(c, content) {
        const char *t = hist_cjson_str(c, "type");
        const char *x = t && strcmp(t, "text") == 0 ? hist_cjson_str(c, "text") : NULL;
        total += (x ? strlen(x) : (t ? strlen(t) + 2 : 0)) + 1;
    }
    char *out = arena_alloc(scratch, total);
    if (!out) return NULL;
    usize at = 0;
    cJSON_ArrayForEach(c, content) {
        const char *t = hist_cjson_str(c, "type");
        const char *x = t && strcmp(t, "text") == 0 ? hist_cjson_str(c, "text") : NULL;
        if (!x && !t) continue;
        if (at) out[at++] = '\n';
        if (x) { usize n = strlen(x); memcpy(out + at, x, n); at += n; }
        else   { at += (usize)snprintf(out + at, total - at, "[%s]", t); }
    }
    out[at] = '\0';
    return at ? out : NULL;
}

static void oc2_tool(BridgeHistory *h, Arena *scratch, cJSON *part, i64 ts_ms) {
    const char *name   = hist_cjson_str(part, "name");
    const char *callid = hist_cjson_str(part, "id");
    cJSON *state = cJSON_GetObjectItemCaseSensitive(part, "state");
    if (!cJSON_IsObject(state)) return;
    BridgeToolKind kind = bridge_tool_kind_for(CHAT_TOOL_OPENCODE, name);

    BridgeEvent call = {0};
    call.type         = BRIDGE_EVENT_TOOL_CALL;
    call.timestamp_ms = ts_ms;
    call.tool_name    = name;
    call.tool_call_id = callid;
    call.tool_kind    = kind;
    call.tool_input   = bridge_json_compact(scratch, cJSON_GetObjectItemCaseSensitive(state, "input"));
    bridge_history_push(h, &call);

    /* A call that never finished has no result at all: an empty one would say
     * the tool returned nothing, which is another fact. */
    const char *status = hist_cjson_str(state, "status");
    if (!status || (strcmp(status, "completed") != 0 && strcmp(status, "error") != 0)) return;
    const char *out = oc2_result(scratch, state);
    if (out && out[0]) {
        BridgeEvent res = {0};
        res.type         = BRIDGE_EVENT_TOOL_RESULT;
        res.timestamp_ms = ts_ms;
        res.tool_name    = name;
        res.tool_call_id = callid;
        res.tool_kind    = kind;
        res.tool_output  = out;
        bridge_history_push(h, &res);
    }
}

static void oc2_row(BridgeHistory *h, Arena *scratch, const char *type, const char *data, i64 ts_ms) {
    if (!type || !data || !data[0]) return;
    cJSON *j = cJSON_Parse(data);
    if (!j) return;

    if (strcmp(type, "user") == 0) {
        const char *text = hist_cjson_str(j, "text");
        if (text && text[0]) bridge_push_message(h, BRIDGE_EVENT_USER, ts_ms, text);
        oc2_files(h, cJSON_GetObjectItemCaseSensitive(j, "files"), ts_ms);

    } else if (strcmp(type, "assistant") == 0) {
        if (!h->source_model) {
            cJSON *model = cJSON_GetObjectItemCaseSensitive(j, "model");
            if (cJSON_IsObject(model)) bridge_history_set_model(h, hist_cjson_str(model, "id"));
        }
        cJSON *p;
        cJSON_ArrayForEach(p, cJSON_GetObjectItemCaseSensitive(j, "content")) {
            const char *pt = hist_cjson_str(p, "type");
            if (!pt) continue;
            if (strcmp(pt, "text") == 0) {
                const char *text = hist_cjson_str(p, "text");
                if (text && text[0]) bridge_push_message(h, BRIDGE_EVENT_ASSISTANT, ts_ms, text);
            } else if (strcmp(pt, "tool") == 0) {
                oc2_tool(h, scratch, p, ts_ms);
            }
            /* reasoning: the thinking channel, dropped as everywhere. */
        }

    } else if (strcmp(type, "compaction") == 0) {
        const char *status = hist_cjson_str(j, "status");
        const char *summary = hist_cjson_str(j, "summary");
        if (status && strcmp(status, "completed") == 0 && summary && summary[0])
            bridge_push_message(h, BRIDGE_EVENT_COMPACTION, ts_ms, summary);
    }
    cJSON_Delete(j);
}

static bool oc2_parse(sqlite3 *db, const ChatSessionMeta *m, BridgeHistory *h) {
    {
        sqlite3_stmt *s = NULL;
        if (sqlite3_prepare_v2(db, "SELECT directory, title, model FROM session_v2 WHERE id = ?1",
                               -1, &s, NULL) == SQLITE_OK) {
            sqlite3_bind_text(s, 1, m->session_id, -1, SQLITE_STATIC);
            if (sqlite3_step(s) == SQLITE_ROW) {
                bridge_history_set_cwd  (h, (const char *)sqlite3_column_text(s, 0));
                bridge_history_set_title(h, (const char *)sqlite3_column_text(s, 1));
                /* The model is a JSON object {id, providerID, variant}. */
                const char *mj = (const char *)sqlite3_column_text(s, 2);
                cJSON *mo = mj ? cJSON_Parse(mj) : NULL;
                if (mo) { bridge_history_set_model(h, hist_cjson_str(mo, "id")); cJSON_Delete(mo); }
            }
            sqlite3_finalize(s);
        }
    }

    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT type, time_created, data FROM session_message "
            "WHERE session_id = ?1 AND type IN ('user','assistant','compaction') "
            "ORDER BY seq ASC", -1, &s, NULL) != SQLITE_OK) return false;
    sqlite3_bind_text(s, 1, m->session_id, -1, SQLITE_STATIC);
    Arena scratch = arena_create(MB(8));
    if (!scratch.base) { sqlite3_finalize(s); return false; }
    while (sqlite3_step(s) == SQLITE_ROW) {
        arena_reset(&scratch);
        oc2_row(h, &scratch, (const char *)sqlite3_column_text(s, 0),
                (const char *)sqlite3_column_text(s, 2), (i64)sqlite3_column_int64(s, 1));
    }
    arena_destroy(&scratch);
    sqlite3_finalize(s);
    return h->event_count > 0;
}

bool bridge_parse_opencode(Arena *a, const ChatSessionMeta *m, BridgeHistory *h) {
    if (!m || !m->path || !m->session_id || !h) return false;

    struct stat st;
    if (stat(m->path, &st) != 0 || !S_ISREG(st.st_mode)) return false;

    oc_sqlite_init();

    /* READONLY without `immutable=1`. Both halves matter: read-only so the
     * open never competes with a running OpenCode for the write lock, and NOT
     * immutable so the WAL is read — a session imported or written minutes
     * ago lives entirely in the WAL, and an immutable open would report it as
     * absent. */
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(m->path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return false;
    }
    liu_sqlite_harden_foreign(db);               /* somebody else's file */

    if (opencode_store_is_v2(db)) {
        bool ok = oc2_parse(db, m, h);
        sqlite3_close(db);
        (void)a;
        return ok;
    }

    /* Session metadata. */
    {
        const char *sql =
            "SELECT directory, title, agent, model, version FROM session WHERE id = ?1";
        sqlite3_stmt *s = NULL;
        if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK) {
            sqlite3_bind_text(s, 1, m->session_id, -1, SQLITE_STATIC);
            if (sqlite3_step(s) == SQLITE_ROW) {
                bridge_history_set_cwd  (h, (const char *)sqlite3_column_text(s, 0));
                bridge_history_set_title(h, (const char *)sqlite3_column_text(s, 1));
                bridge_history_set_model(h, (const char *)sqlite3_column_text(s, 3));
            }
            sqlite3_finalize(s);
        }
    }

    /* Messages with their parts, in wall order. One join rather than N+1
     * queries: a long session is thousands of parts and a per-message query
     * would re-plan for each. ORDER BY the message time first so a part whose
     * own timestamp drifted cannot reorder the conversation. */
    const char *sql =
        "SELECT m.id, m.data, m.time_created, p.data, p.time_created "
        "FROM message m LEFT JOIN part p ON p.message_id = m.id "
        "WHERE m.session_id = ?1 "
        "ORDER BY m.time_created ASC, m.id ASC, p.time_created ASC, p.id ASC";
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return false;
    }
    sqlite3_bind_text(s, 1, m->session_id, -1, SQLITE_STATIC);

    Arena scratch = arena_create(MB(8));
    if (!scratch.base) { sqlite3_finalize(s); sqlite3_close(db); return false; }

    char last_msg[128] = {0};
    bool assistant = false;

    while (sqlite3_step(s) == SQLITE_ROW) {
        const char *mid   = (const char *)sqlite3_column_text(s, 0);
        const char *mdata = (const char *)sqlite3_column_text(s, 1);
        i64         mts   = (i64)sqlite3_column_int64(s, 2);
        const char *pdata = (const char *)sqlite3_column_text(s, 3);
        i64         pts   = (i64)sqlite3_column_int64(s, 4);

        if (mid && strcmp(mid, last_msg) != 0) {
            snprintf(last_msg, sizeof last_msg, "%s", mid);
            assistant = false;
            if (mdata) {
                cJSON *mj = cJSON_Parse(mdata);
                if (mj) {
                    const char *role = hist_cjson_str(mj, "role");
                    assistant = role && strcmp(role, "assistant") == 0;
                    if (!h->source_model)
                        bridge_history_set_model(h, hist_cjson_str(mj, "modelID"));
                    cJSON_Delete(mj);
                }
            }
        }

        if (!pdata) continue;   /* LEFT JOIN: a message with no parts yet */
        arena_reset(&scratch);
        oc_part(h, &scratch, pdata, pts > 0 ? pts : mts, assistant);
    }

    arena_destroy(&scratch);
    sqlite3_finalize(s);
    sqlite3_close(db);
    (void)a;
    return h->event_count > 0;
}
