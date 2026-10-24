/* Liu — bounded, asynchronous prompt-attachment thumbnail cache. */
#ifndef UI_PROMPT_THUMB_H
#define UI_PROMPT_THUMB_H

#include "core/types.h"

#define PROMPT_THUMB_MAX_DIM 256
#define PROMPT_THUMB_SLOTS   16

typedef struct {
    const u8 *rgba;
    i32       w, h;
    u64       generation;
    bool      failed;
} PromptThumb;

/* A new path is queued and immediately returns a pixel-less placeholder.
 * Initial/TTL filesystem checks are worker-only; even missing files complete
 * asynchronously. Refresh keeps existing pixels until a new result arrives. */
const PromptThumb *prompt_thumb_get(const char *path);

/* Main-loop integration: install completions and request exactly one redraw. */
bool prompt_thumb_pump(void);
bool prompt_thumb_completion_ready(void);
/* Main-thread publication serial, including results consumed by get()/trim.
 * Lets the process loop invalidate every window sharing the thumbnail cache. */
u64 prompt_thumb_revision(void);

/* Reclaim thumbnails unused for 30 seconds (internally limited to 1 Hz). */
void prompt_thumb_trim_idle(f64 now_sec);

/* Stop/join the worker and release queued/completed/live owners. */
void prompt_thumb_shutdown(void);

#endif /* UI_PROMPT_THUMB_H */
