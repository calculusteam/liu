/*
 * liu-history - Antigravity transcript parser.
 *
 * Antigravity (agy) stores conversations as SQLite databases at
 * ~/.gemini/antigravity/conversations/<uuid>.db. Each DB contains a
 * `steps` table with protobuf-encoded step data and a `trajectory_meta`
 * table with session metadata.
 *
 * parser_antigravity_run() opens the SQLite DB, iterates the steps table,
 * and extracts user/assistant text from the protobuf blobs using a minimal
 * length-delimited field scanner.
 *
 * parser_antigravity_line() is the legacy JSONL line parser for the older
 * code_tracker format (kept for backward compatibility).
 */
#include "history/parser.h"
#include "history/event.h"
#include "history/util.h"
#include "core/memory.h"
#include "sqlite3.h"
#include "core/sqlite_harden.h"

#include "cJSON.h"

#include <string.h>
#include <ctype.h>

#define cjson_str hist_cjson_str

u32 parser_antigravity_line(Arena *a, const char *line, usize len,
                            ChatEvent *out, u32 out_cap) {
    if (!line || !len || !out || !out_cap) return 0;
    if (len > 4 * 1024 * 1024) return 0;

    const char *p = line;
    usize rem = len;
    while (rem && *p != '{') { p++; rem--; }
    if (!rem) return 0;

    cJSON *root = cJSON_ParseWithLength(p, rem);
    if (!root) return 0;

    u32 emitted = 0;

    const char *content = cjson_str(root, "content");
    const char *display = cjson_str(root, "display");
    const char *text = content ? content : display;
    if (!text) goto done;

    ChatRole role = CHAT_ROLE_USER;
    const char *type = cjson_str(root, "type");
    if (type) {
        if (strcmp(type, "user") == 0)           role = CHAT_ROLE_USER;
        else if (strcmp(type, "assistant") == 0) role = CHAT_ROLE_ASSISTANT;
        else                                     goto done;
    }

    i64 ts_ms = 0;
    const char *ts_str = cjson_str(root, "timestamp");
    if (ts_str) {
        ts_ms = hist_parse_iso8601_ms(ts_str);
    } else {
        (void)hist_cjson_i64(root, "timestamp", &ts_ms);
    }

    if (emitted < out_cap) {
        out[emitted++] = (ChatEvent){
            .origin_tool  = CHAT_TOOL_ANTIGRAVITY,
            .role         = role,
            .timestamp_ms = ts_ms,
            .text         = hist_strdup(a, text),
            .tool_name    = NULL,
            .session_id   = NULL,
        };
    }

done:
    cJSON_Delete(root);
    return emitted;
}

/* ------------------------------------------------------------------ */

static usize pb_read_varint(const u8 *data, usize len, usize *pos, u64 *out) {
    u64 result = 0;
    u32 shift = 0;
    while (*pos < len && shift < 64) {
        u8 b = data[*pos];
        (*pos)++;
        result |= (u64)(b & 0x7F) << shift;
        if (!(b & 0x80)) {
            if (out) *out = result;
            return 1;
        }
        shift += 7;
    }
    return 0;
}

/* Charges each scanned byte against *budget and bails (as "not text") once it
 * is spent, so a crafted blob can't drive an unbounded number of full-length
 * scans (see pb_extract_longest_text). */
static bool is_text_run(const u8 *s, usize len, usize *budget) {
    if (len < 16) return false;
    usize printable = 0;
    for (usize i = 0; i < len; i++) {
        if (*budget == 0) return false;
        (*budget)--;
        u8 c = s[i];
        if (c >= 0x20 && c < 0x7F) printable++;
        else if (c == '\n' || c == '\r' || c == '\t') printable++;
        else if (c >= 0xC2 && c <= 0xF4) {
            /* A whole UTF-8 sequence is one printable character, so its
             * continuation bytes (0x80-0xBF) do not end the run; otherwise a
             * Turkish, CJK or emoji prompt would never be taken as text and
             * an id or a path would be shown in its place. */
            usize need = c >= 0xF0 ? 3 : (c >= 0xE0 ? 2 : 1);
            if (i + need >= len) return false;   /* cut mid-character */
            for (usize k = 1; k <= need; k++)
                if ((s[i + k] & 0xC0) != 0x80) return false;
            if (*budget < need) return false;
            *budget -= need;
            i += need;
            printable += need + 1;
        }
        else return false;
    }
    return printable > len * 3 / 4;
}

static const char *pb_extract_longest_text_depth(Arena *a, const u8 *data,
                                                  usize len, u32 depth, usize *budget) {
    if (!a || !data || depth > 64) return NULL;
    const char *best = NULL;
    usize best_len = 0;

    usize pos = 0;
    while (pos < len) {
        /* Shared work budget across the whole recursion: without it a hostile
         * blob nested ~64 deep, each level wrapping a large run of "allowed"
         * bytes, makes is_text_run re-scan the same region at every level
         * (O(depth × payload) — a seconds-to-minutes hang). When spent, stop
         * and return the best text found so far. */
        if (*budget == 0) break;
        (*budget)--;
        u64 tag;
        if (!pb_read_varint(data, len, &pos, &tag)) break;
        u32 wire = (u32)(tag & 0x07);

        if (wire == 2) {
            u64 slen;
            if (!pb_read_varint(data, len, &pos, &slen)) break;
            if (slen > (u64)(len - pos)) break;
            if (slen >= 16 && is_text_run(data + pos, (usize)slen, budget)) {
                if ((usize)slen > best_len) {
                    best = hist_strndup(a, (const char *)(data + pos), (usize)slen);
                    best_len = (usize)slen;
                }
            } else if (slen > 8) {
                const char *nested = pb_extract_longest_text_depth(
                    a, data + pos, (usize)slen, depth + 1u, budget);
                if (nested) {
                    usize nlen = strlen(nested);
                    if (nlen > best_len) {
                        best = nested;
                        best_len = nlen;
                    }
                }
            }
            pos += (usize)slen;
        } else if (wire == 0) {
            u64 dummy;
            if (!pb_read_varint(data, len, &pos, &dummy)) break;
        } else if (wire == 5) {
            if (pos + 4 > len) break;
            pos += 4;
        } else if (wire == 1) {
            if (pos + 8 > len) break;
            pos += 8;
        } else {
            break;
        }
    }
    return best;
}

static const char *pb_extract_longest_text(Arena *a, const u8 *data, usize len) {
    /* Bound total scan work at a generous multiple of the blob size: well-formed
     * payloads scan roughly O(len) and never trip it, while an adversarially
     * nested blob is capped instead of running to O(depth × len). */
    usize budget = (len <= SIZE_MAX / 8) ? len * 8 : SIZE_MAX;
    return pb_extract_longest_text_depth(a, data, len, 0, &budget);
}

u64 parser_antigravity_run(Arena *a, const char *path,
                           chat_event_cb cb, void *user) {
    if (!a || !path || !cb) return 0;

    /* sqlite3_initialize is idempotent and thread-safe; relying on it
     * rather than an unsynchronised static flag avoids a data race
     * between viewers. */
    (void)sqlite3_initialize();

    sqlite3 *db = NULL;
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return 0;
    }
    liu_sqlite_harden_foreign(db);               /* somebody else's file */

    const char *sql =
        "SELECT s.idx, s.step_type, s.step_payload "
        "FROM steps s ORDER BY s.idx ASC";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return 0;
    }

    u64 total = 0;
    bool prev_was_user = false;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        i32 step_type = sqlite3_column_int(stmt, 1);
        const u8 *payload = (const u8 *)sqlite3_column_blob(stmt, 2);
        i32 payload_len = sqlite3_column_bytes(stmt, 2);

        if (!payload || payload_len <= 0 || payload_len > 64 * 1024 * 1024)
            continue;

        const char *text = pb_extract_longest_text(a, payload, (usize)payload_len);
        if (!text) continue;

        ChatRole role;
        if (!prev_was_user) {
            role = CHAT_ROLE_USER;
            prev_was_user = true;
        } else {
            role = CHAT_ROLE_ASSISTANT;
            prev_was_user = false;
        }

        (void)step_type;

        ChatEvent ev = {
            .origin_tool  = CHAT_TOOL_ANTIGRAVITY,
            .role         = role,
            .timestamp_ms = 0,
            .text         = text,
            .tool_name    = NULL,
            .session_id   = NULL,
        };
        total++;
        if (!cb(&ev, user)) break;
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return total;
}
