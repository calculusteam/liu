/*
 * liu-history/bridge - source dispatch and the ChatEvent bridge.
 *
 * bridge_parse_source() is the single entry point every caller uses. It picks
 * between a bridge-native parser and the ChatEvent fold per tool, and that
 * choice is the interesting part of this file:
 *
 *   Claude / Codex / Grok get bridge-native re-reads, because their
 *   ChatEvent parsers cannot express what a resume needs. parser_claude.c
 *   writes a TOOL_USE's NAME into `tool_name` and a TOOL_RESULT's tool_use_id
 *   into the SAME field, so once an event is a ChatEvent the pairing is gone;
 *   parser_codex.c does the same with call_id, and additionally flattens a
 *   function_call's `arguments` into `text`, which is fine for a viewer and
 *   useless for reconstructing a call. Grok stores its calls in a sibling
 *   `tool_calls` array the ChatEvent parser folds into prose.
 *
 *   Copilot / CommandCode / Antigravity / Cline / Cline CLI go through the
 *   fold. Their ChatEvent parsers already emit calls and results as separate
 *   events in order, so the pairing is recoverable by adjacency, and the
 *   alternative — a second parser per tool — is exactly the duplication the
 *   IR exists to avoid.
 */
#include "history/bridge/sources.h"
#include "history/bridge/bridge.h"
#include "history/bridge/tool_map.h"
#include "history/parser.h"
#include "history/util.h"

#include "cJSON.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------- utilities -- */

const char *bridge_json_compact(Arena *a, void *node_cjson) {
    cJSON *node = (cJSON *)node_cjson;
    if (!node) return NULL;
    if (cJSON_IsString(node))
        return node->valuestring && node->valuestring[0] ? node->valuestring : NULL;
    return hist_cjson_serialize_compact(a, node);
}

/* Bytes a base64 string of `len` characters decodes to (padding counted). */
static usize base64_decoded_len(const char *b64, usize len) {
    usize pad = 0;
    while (pad < 2 && pad < len && b64[len - 1 - pad] == '=') pad++;
    usize n = len / 4 * 3 + (len % 4 ? len % 4 - 1 : 0);
    return n > pad ? n - pad : 0;
}

bool bridge_media_note(void *part_cjson, char *out, usize cap) {
    cJSON *part = (cJSON *)part_cjson;
    if (!out || cap == 0) return false;
    out[0] = '\0';
    const char *pt = part ? hist_cjson_str(part, "type") : NULL;
    if (!pt) return false;

    const char *what;
    if (strcmp(pt, "image") == 0 || strcmp(pt, "input_image") == 0) what = "image";
    else if (strcmp(pt, "document") == 0)                           what = "document";
    else return false;

    const char *media = NULL, *data = NULL, *url = NULL;
    cJSON *src = cJSON_GetObjectItemCaseSensitive(part, "source");
    if (cJSON_IsObject(src)) {            /* Anthropic: {source:{media_type,data|url}} */
        media = hist_cjson_str(src, "media_type");
        data  = hist_cjson_str(src, "data");
        url   = hist_cjson_str(src, "url");
    } else {                              /* Responses: {image_url:"data:…"|"https:…"} */
        url = hist_cjson_str(part, "image_url");
    }

    /* A data: URL is the payload again, with its type in front. */
    char mbuf[64];
    if (!data && url && strncmp(url, "data:", 5) == 0) {
        const char *semi  = strchr(url, ';');
        const char *comma = strchr(url, ',');
        if (comma) {
            usize ml = (usize)((semi && semi < comma ? semi : comma) - (url + 5));
            if (ml > 0 && ml < sizeof mbuf) {
                memcpy(mbuf, url + 5, ml);
                mbuf[ml] = '\0';
                media = mbuf;
            }
            data = comma + 1;
        }
        url = NULL;
    }

    /* The type is the source's text and goes into a message: letters, digits
     * and the few marks a media type has, or it is not named at all. */
    if (media) {
        usize ok = strspn(media, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                 "0123456789/+.-");
        if (ok == 0 || media[ok] != '\0' || ok >= sizeof mbuf) media = NULL;
    }

    if (data)
        snprintf(out, cap, "[%s: %s, %zu bytes]", what, media ? media : "unknown type",
                 base64_decoded_len(data, strlen(data)));
    else if (url && url[0]) {
        usize n = strlen(url);
        if (n > 160) {
            n = 160;
            while (n > 0 && ((unsigned char)url[n] & 0xC0) == 0x80) n--;
        }
        snprintf(out, cap, "[%s: %.*s]", what, (int)n, url);
    } else
        snprintf(out, cap, "[%s]", what);
    return true;
}

/* Join an Anthropic content array into one text: its `text` blocks as they
 * are and, in their place in the order, the note that stands for each image
 * or document (`*media` counts those). Used where a target wants one message
 * rather than one event per block. NULL when the array holds neither. */
static char *join_blocks(Arena *a, cJSON *content, u32 *media) {
    *media = 0;
    if (!content || !cJSON_IsArray(content)) return NULL;
    char note[256];
    usize total = 0;
    cJSON *it = NULL;
    cJSON_ArrayForEach(it, content) {
        const char *tp = hist_cjson_str(it, "type");
        if (tp && strcmp(tp, "text") == 0) {
            const char *tx = hist_cjson_str(it, "text");
            if (tx) total += strlen(tx) + 1;
        } else if (bridge_media_note(it, note, sizeof note)) {
            total += strlen(note) + 1;
        }
    }
    if (!total) return NULL;
    char *buf = arena_alloc(a, total + 1);
    if (!buf) return NULL;
    usize off = 0;
    cJSON_ArrayForEach(it, content) {
        const char *tp = hist_cjson_str(it, "type");
        const char *tx = NULL;
        if (tp && strcmp(tp, "text") == 0) {
            tx = hist_cjson_str(it, "text");
        } else if (bridge_media_note(it, note, sizeof note)) {
            tx = note;
            (*media)++;
        }
        if (tx) {
            usize n = strlen(tx);
            if (off > 0 && off < total) buf[off++] = '\n';
            memcpy(buf + off, tx, n);
            off += n;
        }
    }
    buf[off] = '\0';
    return buf;
}

u32 bridge_push_anthropic_content(BridgeHistory *h, ChatTool src,
                                  void *content_cjson, BridgeEventType role_type,
                                  i64 ts_ms) {
    cJSON *content = (cJSON *)content_cjson;
    if (!h || !content) return 0;
    u32 n = 0;

    if (cJSON_IsString(content)) {
        if (bridge_push_message(h, role_type, ts_ms, content->valuestring)) n++;
        return n;
    }
    if (!cJSON_IsArray(content)) return 0;

    cJSON *part = NULL;
    cJSON_ArrayForEach(part, content) {
        const char *pt = hist_cjson_str(part, "type");
        if (!pt) continue;

        if (strcmp(pt, "text") == 0) {
            if (bridge_push_message(h, role_type, ts_ms,
                                    hist_cjson_str(part, "text"))) n++;

        } else if (strcmp(pt, "thinking") == 0 || strcmp(pt, "redacted_thinking") == 0 ||
                   strcmp(pt, "reasoning") == 0) {
            /* Dropped on purpose — see sources.h. */
            continue;

        } else if (strcmp(pt, "image") == 0 || strcmp(pt, "document") == 0) {
            /* What the user attached cannot be handed to another agent. The
             * turn keeps its place as a note that says so, and the loss is
             * counted, so a conversation about a screenshot does not arrive
             * with no sign that there had been one. */
            char note[256];
            if (bridge_media_note(part, note, sizeof note) &&
                bridge_push_message(h, role_type, ts_ms, note)) n++;
            h->fidelity.events_dropped++;
            h->fidelity.history_complete = false;

        } else if (strcmp(pt, "tool_use") == 0) {
            const char *name = hist_cjson_str(part, "name");
            const char *id   = hist_cjson_str(part, "id");
            cJSON *input = cJSON_GetObjectItemCaseSensitive(part, "input");
            BridgeEvent e = {0};
            e.type         = BRIDGE_EVENT_TOOL_CALL;
            e.timestamp_ms = ts_ms;
            e.tool_name    = name;
            e.tool_call_id = id;
            e.tool_kind    = bridge_tool_kind_for(src, name);
            e.tool_input   = bridge_json_compact(h->arena, input);
            if (bridge_history_push(h, &e)) n++;

        } else if (strcmp(pt, "tool_result") == 0) {
            const char *id = hist_cjson_str(part, "tool_use_id");
            cJSON *inner = cJSON_GetObjectItemCaseSensitive(part, "content");
            const char *txt = NULL;
            u32 media = 0;
            if (cJSON_IsString(inner)) {
                txt = inner->valuestring;
            } else if (cJSON_IsArray(inner)) {
                /* A result that is an image (a screenshot, Read of a PNG) is
                 * its note. Serialised as JSON it would be kilobytes of
                 * base64 under a truncation marker. */
                txt = join_blocks(h->arena, inner, &media);
                if (!txt) txt = hist_cjson_serialize_compact(h->arena, inner);
            } else if (inner) {
                txt = hist_cjson_serialize_compact(h->arena, inner);
            }
            BridgeEvent e = {0};
            e.type         = BRIDGE_EVENT_TOOL_RESULT;
            e.timestamp_ms = ts_ms;
            e.tool_call_id = id;
            e.tool_output  = txt;
            /* The note is less than the tool returned: marked and counted the
             * way a clipped output is. */
            e.tool_output_truncated = media > 0;
            /* A result carries no tool name in the Anthropic shape; the name
             * lives on the call it points at. Leaving it NULL is correct —
             * targets that need one look it up by call id. */
            if (bridge_history_push(h, &e)) n++;
        }
    }
    return n;
}

/* --------------------------------------------------- ChatEvent -> IR fold - */

typedef struct {
    BridgeHistory *h;
    ChatTool       tool;
    /* The last TOOL_USE's name and synthesized id, so the TOOL_RESULT that
     * follows can be paired with it. ChatEvent puts the call id in the same
     * field as the tool name, so this adjacency is the only signal left. */
    char  last_call_name[128];
    char  last_call_id[128];
    bool  have_open_call;
    u32   pushed;
} FoldCtx;

static bool fold_cb(const ChatEvent *e, void *user) {
    FoldCtx *c = user;
    BridgeHistory *h = c->h;

    switch (e->role) {
        case CHAT_ROLE_USER:
            if (bridge_push_message(h, BRIDGE_EVENT_USER, e->timestamp_ms, e->text))
                c->pushed++;
            break;

        case CHAT_ROLE_ASSISTANT:
            if (bridge_push_message(h, BRIDGE_EVENT_ASSISTANT, e->timestamp_ms, e->text))
                c->pushed++;
            break;

        case CHAT_ROLE_TOOL_USE: {
            /* ChatEvent's `text` for a call is the serialized arguments (every
             * parser in the tree does this) and `tool_name` is the name. */
            BridgeEvent b = {0};
            b.type         = BRIDGE_EVENT_TOOL_CALL;
            b.timestamp_ms = e->timestamp_ms;
            b.tool_name    = e->tool_name;
            b.tool_kind    = bridge_tool_kind_for(c->tool, e->tool_name);
            b.tool_input   = e->text;
            if (bridge_history_push(h, &b)) {
                c->pushed++;
                /* Remember what ir.c synthesized so the result can point at
                 * it. Reading it back out of the history is the only way to
                 * see the id — the caller never supplies one here. */
                const BridgeEvent *stored = &h->events[h->event_count - 1];
                hist_copy_bounded(c->last_call_id, sizeof c->last_call_id,
                                  stored->tool_call_id ? stored->tool_call_id : "");
                hist_copy_bounded(c->last_call_name, sizeof c->last_call_name,
                                  e->tool_name ? e->tool_name : "");
                c->have_open_call = true;
            }
            break;
        }

        case CHAT_ROLE_TOOL_RESULT: {
            BridgeEvent b = {0};
            b.type         = BRIDGE_EVENT_TOOL_RESULT;
            b.timestamp_ms = e->timestamp_ms;
            b.tool_output  = e->text;
            if (c->have_open_call) {
                b.tool_call_id = c->last_call_id;
                b.tool_name    = c->last_call_name;
                b.tool_kind    = bridge_tool_kind_for(c->tool, c->last_call_name);
                c->have_open_call = false;
            }
            /* No open call: an orphan result. Kept rather than dropped — the
             * OUTPUT is real conversation context even when the call that
             * produced it fell outside the parsed window (a compacted
             * transcript starts mid-turn all the time). ir.c gives it a fresh
             * synthetic id; targets that require pairing render it as a
             * standalone record. */
            if (bridge_history_push(h, &b)) c->pushed++;
            break;
        }

        case CHAT_ROLE_SYSTEM:
        default:
            /* System rows in these stores are the tool's own scaffolding
             * (reasoning summaries, permission flips, snapshots). Replaying
             * one agent's scaffolding into another is noise at best; the one
             * exception is a COMPACTION summary, which the parsers do not
             * currently distinguish, so nothing is emitted here. */
            break;
    }
    return true;
}

u32 bridge_source_from_chatevents(Arena *arena, const ChatSessionMeta *meta,
                                  BridgeHistory *h) {
    if (!arena || !meta || !h || !meta->path) return 0;

    FoldCtx ctx = {0};
    ctx.h    = h;
    ctx.tool = meta->tool;

    /* parser_run_file resets `arena` after every line, which would free the
     * strings the IR just dup'd out of it. So it gets its own scratch arena
     * and the IR keeps the caller's. Sized from the file for the whole-file
     * parsers (cline family, antigravity), which slurp before they parse. */
    usize scratch = MB(4);
    if (meta->size_bytes > 0) {
        usize want = (usize)meta->size_bytes * 3u + MB(4);
        if (want > scratch) scratch = want;
        if (scratch > MB(192)) scratch = MB(192);
    }
    Arena tmp = arena_create(scratch);
    if (!tmp.base) return 0;
    parser_run_file(&tmp, meta->tool, meta->path, fold_cb, &ctx);
    arena_destroy(&tmp);
    return ctx.pushed;
}

/* ---------------------------------------------------------- source dispatch */

bool bridge_source_supported(ChatTool tool) {
    switch (tool) {
        case CHAT_TOOL_CLAUDE:
        case CHAT_TOOL_CODEX:
        case CHAT_TOOL_COPILOT:
        case CHAT_TOOL_ANTIGRAVITY:
        case CHAT_TOOL_XAI:
        case CHAT_TOOL_COMMANDCODE:
        case CHAT_TOOL_CLINE:
        case CHAT_TOOL_ROO:
        case CHAT_TOOL_KILO:
        case CHAT_TOOL_CLINE_CLI:
        case CHAT_TOOL_OPENCODE:
        case CHAT_TOOL_KIMI:
        case CHAT_TOOL_CURSOR:
        case CHAT_TOOL_QWEN:
        case CHAT_TOOL_AMP:
        case CHAT_TOOL_KIRO:
        case CHAT_TOOL_DROID:
        case CHAT_TOOL_PI:
        case CHAT_TOOL_OMP:
            return true;
        default:
            return false;
    }
}

bool bridge_parse_source(Arena *arena, const ChatSessionMeta *meta,
                         BridgeHistory *out) {
    if (!arena || !meta || !out) return false;

    bridge_history_init(out, arena, meta->tool);
    bridge_history_set_session_id(out, meta->session_id);
    bridge_history_set_path(out, meta->path);
    bridge_history_set_cwd(out, meta->cwd);

    bool ok = false;
    switch (meta->tool) {
        /* Bridge-native: ChatEvent cannot carry the tool pairing for these. */
        case CHAT_TOOL_CLAUDE:      ok = bridge_parse_claude  (arena, meta, out); break;
        case CHAT_TOOL_CODEX:       ok = bridge_parse_codex   (arena, meta, out); break;
        case CHAT_TOOL_XAI:         ok = bridge_parse_grok    (arena, meta, out); break;

        /* Bridge-native: never had a ChatEvent parser at all. */
        case CHAT_TOOL_OPENCODE:    ok = bridge_parse_opencode(arena, meta, out); break;
        case CHAT_TOOL_KIMI:        ok = bridge_parse_kimi    (arena, meta, out); break;
        case CHAT_TOOL_CURSOR:      ok = bridge_parse_cursor  (arena, meta, out); break;
        case CHAT_TOOL_QWEN:        ok = bridge_parse_qwen    (arena, meta, out); break;
        case CHAT_TOOL_AMP:         ok = bridge_parse_amp     (arena, meta, out); break;
        case CHAT_TOOL_KIRO:        ok = bridge_parse_kiro    (arena, meta, out); break;
        case CHAT_TOOL_DROID:       ok = bridge_parse_droid   (arena, meta, out); break;

        /* The fold over the existing ChatEvent parsers. */
        case CHAT_TOOL_COPILOT:
        case CHAT_TOOL_ANTIGRAVITY:
        case CHAT_TOOL_COMMANDCODE:
        case CHAT_TOOL_PI:
        case CHAT_TOOL_OMP:
        case CHAT_TOOL_CLINE:
        case CHAT_TOOL_ROO:
        case CHAT_TOOL_KILO:
        case CHAT_TOOL_CLINE_CLI:
            ok = bridge_source_from_chatevents(arena, meta, out) > 0;
            break;

        default:
            ok = false;
            break;
    }

    /* A title nobody set: use the first user turn, clipped. Every target has
     * a title field and an untitled session is hard to find again. */
    if (ok && !out->title) {
        for (usize i = 0; i < out->event_count; i++) {
            if (out->events[i].type == BRIDGE_EVENT_USER && out->events[i].text) {
                bridge_history_set_title(out, out->events[i].text);
                break;
            }
        }
    }

    bridge_history_finish(out);
    if (!ok || bridge_history_is_empty(out)) {
        bridge_history_free(out);
        return false;
    }
    return true;
}
