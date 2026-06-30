/*
 * liu-history - Copilot CLI JSONL parser.
 *
 * Copilot CLI's session format is less stable than Claude/Codex, so this
 * parser is intentionally heuristic:
 * - detect role from common keys (`role`, `speaker`, `author.role`, `type`)
 * - pull text from common payload keys (`text`, `content`, `message`, etc.)
 * - serialize unknown structured payloads compactly as a last resort
 *
 * This is still far better than the previous raw-preview stub because real
 * user / assistant / tool turns now survive in a normalized form.
 */
#include "history/parser.h"
#include "history/event.h"
#include "history/util.h"

#include "cJSON.h"

#include <string.h>

#define cjstr            hist_cjson_str
#define serialize_compact hist_cjson_serialize_compact

static cJSON *cjitem(cJSON *obj, const char *key) {
    return cJSON_GetObjectItemCaseSensitive(obj, key);
}

static ChatRole role_from_string(const char *s) {
    if (!s) return CHAT_ROLE_SYSTEM;
    if (strcmp(s, "user") == 0 || strcmp(s, "human") == 0) return CHAT_ROLE_USER;
    if (strcmp(s, "assistant") == 0 || strcmp(s, "copilot") == 0 ||
        strcmp(s, "model") == 0) return CHAT_ROLE_ASSISTANT;
    if (strcmp(s, "tool") == 0 || strcmp(s, "tool_use") == 0 ||
        strcmp(s, "tool_call") == 0 || strcmp(s, "function_call") == 0 ||
        strcmp(s, "invocation") == 0) return CHAT_ROLE_TOOL_USE;
    if (strcmp(s, "tool_result") == 0 || strcmp(s, "tool_output") == 0 ||
        strcmp(s, "function_result") == 0 || strcmp(s, "observation") == 0 ||
        strcmp(s, "execution_result") == 0) return CHAT_ROLE_TOOL_RESULT;
    return CHAT_ROLE_SYSTEM;
}

static bool is_text_key(const char *key) {
    static const char *keys[] = {
        "text", "content", "body", "message", "prompt", "response",
        "result", "output", "input", "value", "excerpt", "arguments",
    };
    for (usize i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (strcmp(key, keys[i]) == 0) return true;
    }
    return false;
}

static bool is_array_key(const char *key) {
    static const char *keys[] = {
        "parts", "items", "messages", "choices", "content", "results",
    };
    for (usize i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (strcmp(key, keys[i]) == 0) return true;
    }
    return false;
}

static usize node_text_len(cJSON *node, int depth) {
    if (!node || depth > 6) return 0;
    if (cJSON_IsString(node) && node->valuestring) return strlen(node->valuestring);

    if (cJSON_IsArray(node)) {
        usize total = 0;
        cJSON *it = NULL;
        cJSON_ArrayForEach(it, node) {
            usize n = node_text_len(it, depth + 1);
            if (n > 0) total += n + 1;
        }
        return total;
    }

    if (!cJSON_IsObject(node)) return 0;

    const char *direct = NULL;
    if ((direct = cjstr(node, "text")) != NULL) return strlen(direct);
    if ((direct = cjstr(node, "body")) != NULL) return strlen(direct);
    if ((direct = cjstr(node, "message")) != NULL) return strlen(direct);
    if ((direct = cjstr(node, "content")) != NULL) return strlen(direct);

    cJSON *child = NULL;
    cJSON_ArrayForEach(child, node) {
        if (child->string && is_text_key(child->string)) {
            usize n = node_text_len(child, depth + 1);
            if (n > 0) return n;
        }
    }
    cJSON_ArrayForEach(child, node) {
        if (child->string && is_array_key(child->string)) {
            usize n = node_text_len(child, depth + 1);
            if (n > 0) return n;
        }
    }
    return 0;
}

static usize node_text_copy(char *dst, usize cap, cJSON *node, int depth) {
    if (!dst || cap == 0 || !node || depth > 6) return 0;

    if (cJSON_IsString(node) && node->valuestring) {
        usize n = strlen(node->valuestring);
        if (n >= cap) n = cap - 1;
        memcpy(dst, node->valuestring, n);
        dst[n] = '\0';
        return n;
    }

    if (cJSON_IsArray(node)) {
        usize off = 0;
        cJSON *it = NULL;
        cJSON_ArrayForEach(it, node) {
            char scratch[8192];
            usize n = node_text_copy(scratch, sizeof(scratch), it, depth + 1);
            if (n == 0) continue;
            if (off > 0 && off + 1 < cap) dst[off++] = '\n';
            usize copy = n;
            if (off + copy >= cap) copy = cap - off - 1;
            memcpy(dst + off, scratch, copy);
            off += copy;
            if (off + 1 >= cap) break;
        }
        dst[off] = '\0';
        return off;
    }

    if (!cJSON_IsObject(node)) return 0;

    const char *direct = NULL;
    if ((direct = cjstr(node, "text")) != NULL ||
        (direct = cjstr(node, "body")) != NULL ||
        (direct = cjstr(node, "message")) != NULL ||
        (direct = cjstr(node, "content")) != NULL) {
        usize n = strlen(direct);
        if (n >= cap) n = cap - 1;
        memcpy(dst, direct, n);
        dst[n] = '\0';
        return n;
    }

    cJSON *child = NULL;
    cJSON_ArrayForEach(child, node) {
        if (child->string && is_text_key(child->string)) {
            usize n = node_text_copy(dst, cap, child, depth + 1);
            if (n > 0) return n;
        }
    }
    cJSON_ArrayForEach(child, node) {
        if (child->string && is_array_key(child->string)) {
            usize n = node_text_copy(dst, cap, child, depth + 1);
            if (n > 0) return n;
        }
    }
    return 0;
}

static char *extract_text(Arena *a, cJSON *node) {
    if (!node) return NULL;
    usize n = node_text_len(node, 0);
    if (n == 0) {
        if (cJSON_IsObject(node) || cJSON_IsArray(node)) return serialize_compact(a, node);
        return NULL;
    }
    char *buf = arena_alloc(a, n + 1);
    if (!buf) return NULL;
    if (node_text_copy(buf, n + 1, node, 0) == 0) return NULL;
    return buf;
}

static cJSON *pick_text_node(cJSON *root, ChatRole role) {
    if (!root) return NULL;

    if (role == CHAT_ROLE_TOOL_USE) {
        cJSON *n = NULL;
        if ((n = cjitem(root, "arguments")) != NULL) return n;
        if ((n = cjitem(root, "input")) != NULL) return n;
        if ((n = cjitem(root, "tool_input")) != NULL) return n;
    } else if (role == CHAT_ROLE_TOOL_RESULT) {
        cJSON *n = NULL;
        if ((n = cjitem(root, "output")) != NULL) return n;
        if ((n = cjitem(root, "result")) != NULL) return n;
        if ((n = cjitem(root, "tool_output")) != NULL) return n;
    }

    const char *keys[] = {
        "message", "content", "text", "body", "prompt",
        "response", "result", "output", "input", "value",
    };
    for (usize i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        cJSON *n = cjitem(root, keys[i]);
        if (n) return n;
    }

    cJSON *author = cjitem(root, "author");
    if (author) {
        cJSON *n = cjitem(author, "content");
        if (n) return n;
    }
    return root;
}

static const char *extract_tool_name(cJSON *root) {
    if (!root) return NULL;
    const char *name = NULL;
    if ((name = cjstr(root, "tool_name")) != NULL) return name;
    if ((name = cjstr(root, "command")) != NULL) return name;
    if ((name = cjstr(root, "function")) != NULL) return name;

    const char *type = cjstr(root, "type");
    if (type && (strstr(type, "tool") || strstr(type, "function"))) {
        if ((name = cjstr(root, "name")) != NULL) return name;
    }

    cJSON *tool = cjitem(root, "tool");
    if (tool && (name = cjstr(tool, "name")) != NULL) return name;
    return NULL;
}

/* GitHub Copilot CLI ("chronicle" event log) wraps every record in a
 *   { "type": "<class.subclass>", "data": { … }, "id": …, "timestamp": …,
 *     "parentId": … }
 * envelope. Roles and payload text live on `data`, not the root, so the
 * legacy generic detector misses ~75% of events. Recognise the envelope by
 * the dotted `type` and route through `data`. */
static cJSON *copilot_envelope_data(cJSON *root) {
    if (!root) return NULL;
    const char *t = cjstr(root, "type");
    if (!t || !strchr(t, '.')) return NULL;          /* not the dotted form */
    cJSON *d = cjitem(root, "data");
    return (d && (cJSON_IsObject(d) || cJSON_IsArray(d))) ? d : NULL;
}

static ChatRole detect_role(cJSON *root) {
    if (!root) return CHAT_ROLE_SYSTEM;

    /* Event-envelope branch — disambiguate by `type` prefix before falling
     * back to the generic key-search heuristics. */
    const char *t = cjstr(root, "type");
    if (t && strchr(t, '.')) {
        cJSON *data = cjitem(root, "data");
        if (strncmp(t, "message.", 8) == 0) {
            const char *r = data ? cjstr(data, "role") : NULL;
            if (r) return role_from_string(r);
            /* `message.user` / `message.assistant` directly encode the role. */
            return role_from_string(t + 8);
        }
        if (strncmp(t, "tool.", 5) == 0) {
            /* "tool.invocation.*" / "tool.call.*" -> we issued the call.
             * "tool.result.*" / "tool.completed" / "tool.output.*" -> we
             * got the response. */
            const char *sub = t + 5;
            if (strncmp(sub, "result", 6) == 0 ||
                strncmp(sub, "completed", 9) == 0 ||
                strncmp(sub, "output", 6) == 0) return CHAT_ROLE_TOOL_RESULT;
            return CHAT_ROLE_TOOL_USE;
        }
    }

    const char *s = NULL;
    if ((s = cjstr(root, "role")) != NULL) return role_from_string(s);
    if ((s = cjstr(root, "speaker")) != NULL) return role_from_string(s);

    cJSON *author = cjitem(root, "author");
    if (author && (s = cjstr(author, "role")) != NULL) return role_from_string(s);

    if ((s = cjstr(root, "type")) != NULL) {
        ChatRole r = role_from_string(s);
        if (r != CHAT_ROLE_SYSTEM) return r;
        if (strstr(s, "assistant")) return CHAT_ROLE_ASSISTANT;
        if (strstr(s, "user")) return CHAT_ROLE_USER;
    }
    return CHAT_ROLE_SYSTEM;
}

static i64 extract_timestamp_ms(cJSON *root) {
    if (!root) return 0;
    const char *keys[] = { "timestamp", "created_at", "time", "date" };
    for (usize i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        cJSON *v = cjitem(root, keys[i]);
        if (!v) continue;
        if (cJSON_IsString(v) && v->valuestring) {
            i64 ms = hist_parse_iso8601_ms(v->valuestring);
            if (ms > 0) return ms;
        } else if (cJSON_IsNumber(v)) {
            double d = v->valuedouble;
            if (d > 1000000000000.0) return (i64)d;
            if (d > 1000000000.0) return (i64)(d * 1000.0);
        }
    }
    return 0;
}

static u32 fallback_preview(Arena *a, const char *line, usize len,
                            ChatEvent *out) {
    usize copy = len < 200 ? len : 200;
    char *preview = arena_alloc(a, copy + 1);
    if (!preview) return 0;
    memcpy(preview, line, copy);
    preview[copy] = '\0';
    out[0] = (ChatEvent){
        .origin_tool = CHAT_TOOL_COPILOT,
        .role        = CHAT_ROLE_SYSTEM,
        .timestamp_ms= 0,
        .text        = preview,
        .tool_name   = NULL,
        .session_id  = NULL,
    };
    return 1;
}

u32 parser_copilot_line(Arena *a, const char *line, usize len,
                        ChatEvent *out, u32 out_cap) {
    if (!line || !len || !out || !out_cap) return 0;
    if (len > 4 * 1024 * 1024) return 0;

    cJSON *root = cJSON_ParseWithLength(line, len);
    if (!root) return fallback_preview(a, line, len, out);

    ChatRole role = detect_role(root);
    i64 t_ms = extract_timestamp_ms(root);
    /* When the line is a dotted-type envelope ({type, data, …}), search the
     * inner `data` first so we pick up real payload text instead of falling
     * back to a serialized envelope. */
    cJSON *payload = copilot_envelope_data(root);
    cJSON *text_node = payload ? pick_text_node(payload, role) : NULL;
    if (!text_node) text_node = pick_text_node(root, role);
    char *text = extract_text(a, text_node);
    char *tool_name = hist_strdup(a, extract_tool_name(payload ? payload : root));

    if (!text && role == CHAT_ROLE_SYSTEM) {
        text = serialize_compact(a, root);
    }

    cJSON_Delete(root);

    if (!text) return fallback_preview(a, line, len, out);
    out[0] = (ChatEvent){
        .origin_tool = CHAT_TOOL_COPILOT,
        .role        = role,
        .timestamp_ms= t_ms,
        .text        = text,
        .tool_name   = tool_name,
        .session_id  = NULL,
    };
    return 1;
}
