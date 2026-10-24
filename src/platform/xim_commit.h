/*
 * Recovering from XBufferOverflow in Xutf8LookupString.
 *
 * Xlib's contract: the RETURN VALUE of an overflowing lookup is the number of
 * bytes the commit needs, and the client is expected to call again with a
 * buffer that size. A commit over the 64-byte stack buffer is ordinary (22
 * three-byte characters, a length any CJK or Hangul candidate list reaches),
 * and dropping it would type nothing, with no error anywhere.
 *
 * This header is the sizing decision alone, so it builds off X11:
 * platform_linux.c cannot be compiled on macOS. The Xlib call SEQUENCE around
 * it (re-looking-up the same event, which is the documented recovery because
 * an overflowing lookup does not consume the IM's compose state) is in
 * platform_linux.c.
 */
#ifndef LIU_XIM_COMMIT_H
#define LIU_XIM_COMMIT_H

#include "core/types.h"

/* Upper bound on one commit. A hostile or broken input method must not turn one
 * keystroke into an unbounded allocation; past this the commit is dropped.
 * Nothing a person types comes close: the longest single commit any real IME
 * produces is a candidate phrase, tens of bytes. */
#define XIM_COMMIT_MAX_BYTES (64 * 1024)

typedef struct {
    bool retry;    /* call the lookup again with `cap` bytes */
    i32  cap;      /* buffer size to allocate, INCLUDING room for a NUL */
} XimRetry;

/* `reported_len` is what the overflowing lookup returned: the bytes needed.
 *
 * `cap` is always at least reported_len + 1. Xlib does not NUL-terminate, so the
 * caller writes the terminator itself and needs the extra byte; sizing exactly
 * to reported_len would be a one-byte overflow. */
static inline XimRetry xim_overflow_retry(i32 reported_len) {
    XimRetry r = { false, 0 };
    if (reported_len <= 0) return r;                  /* nothing to fetch */
    if (reported_len > XIM_COMMIT_MAX_BYTES) return r;
    r.retry = true;
    r.cap   = reported_len + 1;
    return r;
}

#endif /* LIU_XIM_COMMIT_H */
