/*
 * Tab-sleep snapshots serialize a pane's primary screen, scrollback, and
 * terminal state to a file. The format uses Terminal data and stdio without
 * depending on the renderer or window layer.
 *
 * The format is versioned: v1 stores scrollback rows as Cell streams; v2 stores
 * compressed codepoints, RLE attribute runs, and a wide bitmap; v3 adds screen
 * wrap flags and terminal mode, pen, scroll-region, saved-cursor, and keyboard
 * state; v4 stores wide lead words in row metadata. Readers support the earlier
 * layouts.
 */
#ifndef UI_SLEEP_SNAP_H
#define UI_SLEEP_SNAP_H

#include "core/types.h"
#include "terminal/terminal.h"
#include <stdatomic.h>

/* Optional worker control. Cancellation is checked between screen/scrollback
 * records; progress reports the current temporary IO buffers as well as the
 * terminal, so the owning worker can account for growth without UI callbacks.
 * The callbacks are synchronous on the thread performing the snapshot IO. */
typedef struct {
    const atomic_bool *cancel;
    bool preserve_file;          /* transactional batch reader consumes later */
    void (*progress)(void *context, const Terminal *terminal, usize staging_bytes);
    void *context;
} SleepSnapControl;

/* Unique path for this pane snapshot generation, including pid/tab/pane.
 * Delayed cancelled workers cannot overwrite a newer tab or window snapshot. */
void sleep_snap_path(char *out, usize n, i32 tab_idx, i32 pane);
/* Return the private directory used for snapshots. Returns false and
 * leaves the output path empty when it is unavailable; tabs then remain
 * awake. */
bool sleep_snap_dir(char *out, usize n);
/* Remove snapshots belonging to processes that are not running. Called once at
 * launch. */
i32  sleep_snap_sweep_stale(void);

/* Serialise `t`'s primary screen and scrollback to `path`. Returns false and
 * leaves no file behind on any failure — the caller then clears its recorded
 * path, so a wake restores nothing rather than restoring a truncated screen. */
bool sleep_snap_write(const char *path, Terminal *t);

/* Restore into `t`, then DELETE the file. Reading is destructive on purpose:
 * a snapshot is consumed exactly once, and a file left behind after a wake is
 * a leak. */
bool sleep_snap_read(const char *path, Terminal *t);

bool sleep_snap_write_control(const char *path, Terminal *t,
                              const SleepSnapControl *control);
bool sleep_snap_read_control(const char *path, Terminal *t,
                             const SleepSnapControl *control);

#endif /* UI_SLEEP_SNAP_H */
