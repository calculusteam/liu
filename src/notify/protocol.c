/*
 * liu-notify - frame validator (pure, fuzzable).
 */
#include "notify/protocol.h"
#include "core/utf8.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static bool valid_tool(u8 id) {
    switch (id) {
        case TOOL_CLAUDE: case TOOL_COPILOT: case TOOL_CODEX: case TOOL_CUSTOM:
            return true;
        default:
            return false;
    }
}

static bool valid_event(u8 id) {
    return id >= EVT_START && id <= EVT_COMPLETE;
}

/* Packed on the wire, so _Alignof(NotifyHeader) == 1 — we always memcpy,
 * never dereference `buf` as a struct, so alignment does not matter. */
_Static_assert(_Alignof(NotifyHeader) == 1, "header must be byte-aligned on wire");

bool notify_validate(const u8 *buf, size_t len, NotifyFrame *out) {
    /* Layer 0: buffer shape. */
    if (buf == NULL || len < sizeof(NotifyHeader)) return false;
    if (len > NOTIFY_MAX_FRAME) return false;

    NotifyHeader h;
    memcpy(&h, buf, sizeof h);

    /* Layer 1: protocol identity. */
    if (h.magic   != NOTIFY_MAGIC)   return false;
    if (h.version != NOTIFY_VERSION) return false;

    /* Layer 2: enum / flag domain. */
    if (h.flags & ~(u16)FLAG_ALL) return false;
    if (!valid_tool(h.tool_id))   return false;
    if (!valid_event(h.event_id)) return false;

    /* Layer 3: per-field caps. These are the PRIMARY bound on total payload;
     * together they cap payload to 3328 bytes, far below UINT32_MAX. */
    if (h.title_len > NOTIFY_MAX_TITLE) return false;
    if (h.body_len  > NOTIFY_MAX_BODY)  return false;

    /* Layer 4: strict-equality on total length. Rejects trailing junk AND
     * independently guards against a future cap bump that would make the
     * sum arithmetic overflow — both operands already fit in 32 bits but
     * the cast to size_t keeps us safe if NOTIFY_MAX_* grow. */
    size_t total = sizeof(NotifyHeader) + (size_t)h.title_len + (size_t)h.body_len;  /* size_t arithmetic throughout */
    if (total != len) return false;

    const u8 *title = (h.title_len > 0) ? (buf + sizeof(NotifyHeader)) : NULL;
    const u8 *body  = (h.body_len  > 0) ? (buf + sizeof(NotifyHeader) + h.title_len) : NULL;

    /* Layer 5: byte content. */
    if (title) {
        if (!utf8_validate(title, h.title_len))    return false;
        if (memchr(title, 0, h.title_len) != NULL) return false;
    }
    if (body) {
        if (!utf8_validate(body, h.body_len))      return false;
        if (memchr(body, 0, h.body_len) != NULL)   return false;
    }

    if (out) {
        out->header = h;
        out->title  = title;
        out->body   = body;
    }
    return true;
}

const char *notify_event_label(u8 event_id) {
    switch (event_id) {
        case EVT_START:    return "start";
        case EVT_STOP:     return "stop";
        case EVT_NOTIFY:   return "notify";
        case EVT_ERROR:    return "error";
        case EVT_COMPLETE: return "complete";
        default:           return "unknown";
    }
}

const char *notify_tool_label(u8 tool_id) {
    switch (tool_id) {
        case TOOL_CLAUDE:  return "claude";
        case TOOL_COPILOT: return "copilot";
        case TOOL_CODEX:   return "codex";
        case TOOL_CUSTOM:  return "custom";
        default:           return "unknown";
    }
}
