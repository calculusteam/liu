#ifndef HISTORY_FORMATTER_H
#define HISTORY_FORMATTER_H

#include "core/types.h"
#include "history/event.h"

typedef enum {
    HIST_FMT_TEXT = 0,
    HIST_FMT_MARKDOWN,
    HIST_FMT_JSON,
} HistFormat;

typedef enum {
    ROLE_MASK_USER        = 1u << CHAT_ROLE_USER,
    ROLE_MASK_ASSISTANT   = 1u << CHAT_ROLE_ASSISTANT,
    ROLE_MASK_TOOL_USE    = 1u << CHAT_ROLE_TOOL_USE,
    ROLE_MASK_TOOL_RESULT = 1u << CHAT_ROLE_TOOL_RESULT,
    ROLE_MASK_SYSTEM      = 1u << CHAT_ROLE_SYSTEM,
    ROLE_MASK_ALL         = 0xFFu,
} RoleMask;

/* Stateful streaming formatter: init, emit-event per line, finish. */
typedef struct {
    HistFormat fmt;
    u32        role_mask;
    bool       is_tty_stdout;
    u64        event_index;   /* for JSON array comma separation */
} HistFormatter;

void hist_formatter_init(HistFormatter *f, HistFormat fmt, u32 role_mask, bool is_tty);
void hist_formatter_begin(HistFormatter *f);
void hist_formatter_emit(HistFormatter *f, const ChatEvent *e);
void hist_formatter_finish(HistFormatter *f);

#endif
