/*
 * liu-notify - wire protocol
 * Fixed 24-byte binary header, UTF-8 title+body payload. No JSON on the wire.
 */
#ifndef NOTIFY_PROTOCOL_H
#define NOTIFY_PROTOCOL_H

#include "core/types.h"
#include <stddef.h>

#define NOTIFY_MAGIC        0x4E544659u   /* "NTFY" little-endian */
#define NOTIFY_VERSION      1u

#define NOTIFY_MAX_TITLE    256u
#define NOTIFY_MAX_BODY     3072u
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

/* Pointers into the caller-owned buffer after successful validation. */
typedef struct {
    NotifyHeader header;
    const u8    *title;   /* NULL if title_len == 0 */
    const u8    *body;    /* NULL if body_len == 0 */
} NotifyFrame;

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
