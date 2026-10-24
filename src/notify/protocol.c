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

/* Packed on the wire, so _Alignof(NotifyHeader) == 1 — the code always
 * memcpys and never dereferences `buf` as a struct, so alignment does
 * not matter. */
_Static_assert(_Alignof(NotifyHeader) == 1, "header must be byte-aligned on wire");

bool notify_validate(const u8 *buf, size_t len, NotifyFrame *out) {
    /* Layer 0: buffer shape. */
    if (buf == NULL || len < sizeof(NotifyHeader)) return false;
    if (len > NOTIFY_MAX_FRAME) return false;

    NotifyHeader h;
    memcpy(&h, buf, sizeof h);

    /* Layer 1: protocol identity. A v2 frame carries 4 more header bytes, so
     * the version decides how much header there is before the payload. */
    if (h.magic != NOTIFY_MAGIC) return false;
    if (h.version < NOTIFY_VERSION_MIN || h.version > NOTIFY_VERSION) return false;

    size_t hdr_len  = sizeof(NotifyHeader);
    u32    meta_len = 0;
    if (h.version >= 2u) {
        if (len < sizeof(NotifyHeaderV2)) return false;
        NotifyHeaderV2 h2;
        memcpy(&h2, buf, sizeof h2);
        meta_len = h2.meta_len;
        hdr_len  = sizeof(NotifyHeaderV2);
    }

    /* Layer 2: enum / flag domain. */
    if (h.flags & ~(u16)FLAG_ALL) return false;
    if (!valid_tool(h.tool_id))   return false;
    if (!valid_event(h.event_id)) return false;

    /* Layer 3: per-field caps. These are the PRIMARY bound on total payload;
     * together they cap payload to 3840 bytes, far below UINT32_MAX. */
    if (h.title_len > NOTIFY_MAX_TITLE) return false;
    if (h.body_len  > NOTIFY_MAX_BODY)  return false;
    if (meta_len    > NOTIFY_MAX_META)  return false;

    /* Layer 4: strict-equality on total length. Rejects trailing junk AND
     * independently guards against a future cap bump that would make the
     * sum arithmetic overflow — every operand already fits in 32 bits but
     * the cast to size_t keeps the sum safe if NOTIFY_MAX_* grow. */
    size_t total = hdr_len + (size_t)h.title_len + (size_t)h.body_len
                 + (size_t)meta_len;  /* size_t arithmetic throughout */
    if (total != len) return false;

    const u8 *title = (h.title_len > 0) ? (buf + hdr_len) : NULL;
    const u8 *body  = (h.body_len  > 0) ? (buf + hdr_len + h.title_len) : NULL;
    const u8 *meta  = (meta_len    > 0)
                    ? (buf + hdr_len + h.title_len + h.body_len) : NULL;

    /* Layer 5: byte content. */
    if (title) {
        if (!utf8_validate(title, h.title_len))    return false;
        if (memchr(title, 0, h.title_len) != NULL) return false;
    }
    if (body) {
        if (!utf8_validate(body, h.body_len))      return false;
        if (memchr(body, 0, h.body_len) != NULL)   return false;
    }
    if (meta) {
        if (!utf8_validate(meta, meta_len))        return false;
        if (memchr(meta, 0, meta_len) != NULL)     return false;
    }

    if (out) {
        out->header   = h;
        out->title    = title;
        out->body     = body;
        out->meta     = meta;
        out->meta_len = meta_len;
    }
    return true;
}

bool notify_meta_get(const NotifyFrame *f, const char *key, char *out, size_t cap) {
    if (out && cap > 0) out[0] = '\0';
    if (!f || !f->meta || !f->meta_len || !key || !out || cap == 0) return false;

    size_t klen = strlen(key);
    if (klen == 0) return false;

    const u8 *p   = f->meta;
    const u8 *end = f->meta + f->meta_len;
    while (p < end) {
        const u8 *nl = memchr(p, '\n', (size_t)(end - p));
        const u8 *line_end = nl ? nl : end;
        size_t line_len = (size_t)(line_end - p);
        if (line_len > klen && memcmp(p, key, klen) == 0) {
            size_t vlen = line_len - klen;
            if (vlen >= cap) vlen = cap - 1;
            memcpy(out, p + klen, vlen);
            out[vlen] = '\0';
            return vlen > 0;
        }
        if (!nl) break;
        p = nl + 1;
    }
    return false;
}

/* One canonical decimal u32 >= 1 starting at *p; advances *p past it. */
static bool parse_canonical_u32(const char **p, u32 *out) {
    const char *s = *p;
    if (*s < '1' || *s > '9') return false;      /* no leading zero, so no zero */
    u64 v = 0;
    int digits = 0;
    while (*s >= '0' && *s <= '9') {
        if (++digits > 10) return false;         /* u32 has at most 10 */
        v = v * 10u + (u64)(*s - '0');
        s++;
    }
    if (v > 0xFFFFFFFFull) return false;
    *out = (u32)v;
    *p = s;
    return true;
}

bool notify_pane_id_parse(const char *s, u32 *pid, u32 *serial) {
    if (pid)    *pid = 0;
    if (serial) *serial = 0;
    if (!s) return false;
    u32 a = 0, b = 0;
    const char *p = s;
    if (!parse_canonical_u32(&p, &a)) return false;
    if (*p != '.') return false;
    p++;
    if (!parse_canonical_u32(&p, &b)) return false;
    if (*p != '\0') return false;
    if (pid)    *pid = a;
    if (serial) *serial = b;
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
