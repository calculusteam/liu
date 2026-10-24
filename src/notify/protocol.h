/*
 * liu-notify - wire protocol
 * Fixed 24-byte binary header, UTF-8 title+body payload. No JSON on the wire.
 */
#ifndef NOTIFY_PROTOCOL_H
#define NOTIFY_PROTOCOL_H

#include "core/types.h"
#include <stddef.h>

#define NOTIFY_MAGIC        0x4E544659u   /* "NTFY" little-endian */
/* v1 = 24-byte header, title+body only. v2 appends `meta_len` to the header and
 * a meta section after the body, carrying the hook's own identity (which cwd,
 * which agent session) so the GUI can attribute a hook to the pane that
 * produced it instead of guessing from output volume. Senders emit v2; the
 * validator still accepts v1 so an older liu-notify on PATH keeps working. */
#define NOTIFY_VERSION      2u
#define NOTIFY_VERSION_MIN  1u

#define NOTIFY_MAX_TITLE    256u
#define NOTIFY_MAX_BODY     3072u
#define NOTIFY_MAX_META     512u
#define NOTIFY_MAX_FRAME    4096u

typedef enum {
    TOOL_UNKNOWN = 0,
    TOOL_CLAUDE  = 1,
    TOOL_COPILOT = 2,
    TOOL_CODEX   = 3,
    TOOL_CUSTOM  = 255,
} NotifyTool;

typedef enum {
    EVT_NONE     = 0,
    EVT_START    = 1,
    EVT_STOP     = 2,
    EVT_NOTIFY   = 3,
    EVT_ERROR    = 4,
    EVT_COMPLETE = 5,
} NotifyEvent;

typedef enum {
    FLAG_SPEAK = 1u << 0,
    FLAG_BELL  = 1u << 1,
    FLAG_ALL   = FLAG_SPEAK | FLAG_BELL,
} NotifyFlags;

typedef enum {
    SRC_CLI         = 0,
    SRC_STDIN_AUTO  = 1,
    SRC_TEST        = 2,
} NotifySource;

#pragma pack(push, 1)
typedef struct {
    u32 magic;       /* NOTIFY_MAGIC */
    u16 version;     /* NOTIFY_VERSION */
    u16 flags;       /* NotifyFlags; unknown bits => reject */
    u8  tool_id;     /* NotifyTool */
    u8  event_id;    /* NotifyEvent */
    u8  priority;    /* 0..255, reserved */
    u8  source;      /* NotifySource */
    u32 seq;         /* client nonce, distinguishes legit repeats for dedup */
    u32 title_len;   /* <= NOTIFY_MAX_TITLE */
    u32 body_len;    /* <= NOTIFY_MAX_BODY */
} NotifyHeader;
#pragma pack(pop)

_Static_assert(sizeof(NotifyHeader) == 24, "NotifyHeader must be 24 bytes");

/* v2 header: the v1 layout byte-for-byte, plus the meta length. Packed, so the
 * first 24 bytes stay wire-identical and a v1 reader's memcpy of the prefix
 * still lands correctly. */
#pragma pack(push, 1)
typedef struct {
    NotifyHeader base;
    u32 meta_len;    /* <= NOTIFY_MAX_META; 0 when the sender has no identity */
} NotifyHeaderV2;
#pragma pack(pop)

_Static_assert(sizeof(NotifyHeaderV2) == 28, "NotifyHeaderV2 must be 28 bytes");

/* Meta section format: `key=value` lines separated by '\n', no trailing
 * newline, no NUL. Keys currently emitted:
 *   pane=<pid>.<serial>  the Liu pane the hook ran in (see notify_pane_id_parse)
 *   agent=<agent id string, e.g. "grok"; disambiguates TOOL_CUSTOM>
 *   session=<agent's own session id, when its payload carries one>
 *   cwd=<absolute path the hook ran in>
 * Unknown keys are ignored by readers, so new ones are additive; readers find a
 * key by name, never by position. A sender writes a line whole or not at all —
 * a value cut to fit would be a different, valid-looking value. */
#define NOTIFY_META_CWD     "cwd="
#define NOTIFY_META_SESSION "session="
#define NOTIFY_META_AGENT   "agent="
#define NOTIFY_META_PANE    "pane="

/* The environment variable a Liu pane's shell exports, and the longest text a
 * pane id can be: two u32s in decimal and the dot ("4294967295.4294967295"). */
#define NOTIFY_PANE_ENV      "LIU_PANE_ID"
#define NOTIFY_PANE_ID_CAP   24

/* Pointers into the caller-owned buffer after successful validation. */
typedef struct {
    NotifyHeader header;
    const u8    *title;   /* NULL if title_len == 0 */
    const u8    *body;    /* NULL if body_len == 0 */
    const u8    *meta;    /* NULL if meta_len == 0 (always NULL for a v1 frame) */
    u32          meta_len;
} NotifyFrame;

/* Copy the value of `key` (one of the NOTIFY_META_* prefixes) out of a
 * validated frame's meta section into `out` as a NUL-terminated string.
 * Returns false (and writes an empty string) when the key isn't present. */
bool notify_meta_get(const NotifyFrame *f, const char *key, char *out, size_t cap);

/* Parse a pane id, `<pid>.<serial>`: the Liu process id and that process's
 * session serial, both decimal u32 >= 1 in canonical form (no sign, no leading
 * zero, no space, nothing after). Returns true and fills both on success; on
 * anything else returns false and writes 0 to both, which is also what "no
 * pane" looks like to every consumer. Strictness is the point: the id picks
 * the pane a notification is attributed to, so a damaged one must read as
 * absent, never as some other pane. Pure; `pid`/`serial` may be NULL. */
bool notify_pane_id_parse(const char *s, u32 *pid, u32 *serial);

/*
 * Pure, no-side-effect validator. Fuzzable in isolation.
 * Returns true if buf[0..len) is a well-formed notify frame and fills *out.
 * Never writes through `buf`. `out` may be NULL if caller only wants bool.
 */
bool notify_validate(const u8 *buf, size_t len, NotifyFrame *out);

/* Returns a short human label for an event id (for logs and speech prefix). */
const char *notify_event_label(u8 event_id);

/* Returns a short human label for a tool id. */
const char *notify_tool_label(u8 tool_id);

/* Single-byte ack codes written back to the client after frame receipt. */
#define NOTIFY_ACK_OK        0x00u   /* queued, will be spoken */
#define NOTIFY_ACK_REJECT    0x01u   /* bad frame / validation failed */
#define NOTIFY_ACK_DROPPED   0x02u   /* valid but dropped (rate, dedup, full) */

#endif /* NOTIFY_PROTOCOL_H */
