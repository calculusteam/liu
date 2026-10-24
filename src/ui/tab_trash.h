/*
 * tab_trash — defer heavy Terminal/Session teardown to a background thread.
 *
 * `terminal_destroy()` walks every scrollback row and free()'s its cell
 * buffer; for a 10k-line scrollback that is a long run of free() under
 * libmalloc's lock. `session_destroy()` for SSH joins the connect worker,
 * which can stall on a slow socket. Running either inline blocks the main
 * loop long enough to be felt as a freeze during tab close/drag-reorder.
 *
 * Push the doomed pair here instead; a single worker thread drains the
 * queue and calls the real destroy functions out of band.
 */
#ifndef UI_TAB_TRASH_H
#define UI_TAB_TRASH_H

/* An ITEM cap as well as a byte cap. The byte cap alone bounds one dimension of
 * the queue; the worker is serial and each teardown of a deep scrollback takes
 * a while, so a burst of cheap terminals can still put seconds of latency
 * between a close and the memory coming back. */
#ifndef TAB_TRASH_MAX_ITEMS
#define TAB_TRASH_MAX_ITEMS 32
#endif
#ifndef TAB_TRASH_MAX_BYTES
#define TAB_TRASH_MAX_BYTES ((usize)256 * 1024 * 1024)
#endif


#include "core/types.h"

struct Terminal;
struct Session;

void tab_trash_init(void);
void tab_trash_shutdown(void);

/* A queued Terminal still retains its scrollback and is no longer included
 * in the per-frame tab walk. The queue reports those bytes as a transient
 * adjustment until the worker frees them, keeping memory accounting accurate
 * during mass close.
 *
 * The queue is bounded by TAB_TRASH_MAX_BYTES and TAB_TRASH_MAX_ITEMS. If
 * either cap is reached, the caller destroys the item inline; this bounds
 * retained memory while allowing close operations to proceed on a single
 * worker. */
bool tab_trash_defer(struct Terminal *t, struct Session *s);
/* Successful transfer may start local-child shutdown immediately; its final
 * group signal and reap remain with the queue's normal destroy call. */

/* What the queue is holding right now: items waiting, bytes retained, and
 * the age of the oldest item in seconds. For LIU_MEM_PROBE; any pointer
 * may be NULL. */
void tab_trash_stats(i32 *out_depth, usize *out_bytes, f64 *out_oldest_sec);

#endif
