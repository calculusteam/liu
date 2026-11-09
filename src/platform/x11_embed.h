#ifndef PLATFORM_X11_EMBED_H
#define PLATFORM_X11_EMBED_H

/*
 * What a native child of a Liu window needs on X11 (the browser's page, a
 * toolkit's window embedded in it): the display and the window it goes in,
 * and a second event source that the loop's wait drives with its own — the
 * toolkit's main context, whose descriptors and timeouts are not Liu's.
 */

#include "platform/platform.h"

#include <poll.h>

typedef struct {
    /* Fills up to `cap` descriptors to wait on and may lower *timeout_ms
     * (-1 = none of its own); returns how many it filled. */
    i32  (*prepare)(void *ctx, struct pollfd *fds, i32 cap, i32 *timeout_ms);
    /* After the wait, with what it saw on those descriptors; always called
     * after a prepare. */
    void (*dispatch)(void *ctx, const struct pollfd *fds, i32 n);
    /* Without a wait: whatever is ready now. Called on every pass. */
    void (*pump)(void *ctx);
    void *ctx;
} PlatformPollSource;

/* NULL removes it. */
void platform_x11_set_poll_source(const PlatformPollSource *src);
/* The app's own Display *, NULL before platform_init. */
void *platform_x11_display(void);
/* The window's XID, 0 when it has none. */
unsigned long platform_x11_window(PlatformWindow *w);
/* The window that takes its keys (a child of it), the one to give the X focus
 * back to; 0 when it has none. */
unsigned long platform_x11_key_sink(PlatformWindow *w);
/* Events for a window that is not one of Liu's (an embedded toolkit's socket):
 * `fn` gets the XEvent * and returns true when it took it. NULL removes it. */
void platform_x11_set_child_event_handler(bool (*fn)(void *xevent, void *ctx), void *ctx);

#endif
