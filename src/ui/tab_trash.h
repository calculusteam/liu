/*
 * tab_trash — defer heavy Terminal/Session teardown to a background thread.
 *
 * `terminal_destroy()` walks every scrollback row and free()'s its cell
 * buffer; for 10k-line scrollbacks that's ~10-50 ms of free() under
 * libmalloc's lock. `session_destroy()` for SSH joins the connect worker,
 * which can stall on a slow socket. Running either inline blocks the main
 * loop long enough to be felt as a freeze during tab close/drag-reorder.
 *
 * Push the doomed pair here instead; a single worker thread drains the
 * queue and calls the real destroy functions out of band.
 */
#ifndef UI_TAB_TRASH_H
#define UI_TAB_TRASH_H

#include "core/types.h"

struct Terminal;
struct Session;

void tab_trash_init(void);
void tab_trash_shutdown(void);

/* Hand off ownership of `t` and `s` (either may be NULL). Returns true if
 * the items were queued; on failure the caller must destroy them inline. */
bool tab_trash_defer(struct Terminal *t, struct Session *s);

#endif
