/* Owned asynchronous tab snapshot IO. No Session, AppState or UI owner is
 * retained by these jobs. Enqueue, poll, take and release never join disk IO. */
#ifndef UI_SLEEP_IO_H
#define UI_SLEEP_IO_H
#include "core/types.h"
#include "terminal/terminal.h"

#define SLEEP_IO_MAX_PANES 8
#define SLEEP_IO_MAX_JOBS 2
#define SLEEP_IO_MAX_BYTES MB(256)

typedef struct SleepIOJob SleepIOJob;
typedef enum { SLEEP_IO_WRITE, SLEEP_IO_READ, SLEEP_IO_RESIZE } SleepIOKind;
typedef void (*SleepIOWakeFn)(void);

typedef struct {
    const char *path;              /* copied at enqueue; empty READ = blank */
    Terminal *terminal;            /* WRITE/RESIZE: transferred on success */
    i32 cols, rows;                /* READ/RESIZE: exact pane geometry */
    i32 scrollback_lines;
    bool bidi_enabled;
} SleepIOPane;

typedef struct {
    SleepIOKind kind;
    i32 count;
    bool cancelled;
    bool ok[SLEEP_IO_MAX_PANES];
    Terminal *terminals[SLEEP_IO_MAX_PANES]; /* READ/RESIZE or failed WRITE ownership; failed READ = NULL */
    usize terminal_bytes, scrollback_bytes;
} SleepIOResult;

/* Set once at startup; each job copies the wake function before starting.
 * It must only wake the event loop and must not access a window or AppState. */
void sleep_io_set_wake(SleepIOWakeFn wake);

/* NULL means no ownership changed. Admission bounds outstanding jobs and
 * estimated bytes, including completed results waiting to be collected.
 * A single oversized job may run alone so a large tab can always restore.
 * WRITE is atomic across panes: any IO failure keeps all original terminals
 * for return and removes partial files; success destroys input terminals.
 * Successful READ consumes its path; failed READ preserves valid snapshots
 * for retry, while cancellation/discard removes unclaimed paths.
 * WRITE/RESIZE detach Terminal UI callbacks on successful ownership transfer;
 * the UI configures callbacks again after adopting a READ/RESIZE result. */
SleepIOJob *sleep_io_write(const SleepIOPane *panes, i32 count);
SleepIOJob *sleep_io_read(const SleepIOPane *panes, i32 count);
SleepIOJob *sleep_io_resize(const SleepIOPane *panes, i32 count);

bool sleep_io_ready(const SleepIOJob *job);
/* A successful take is the only operation that moves output terminals to the
 * caller. Their MEM_TERMINAL/MEM_SCROLLBACK charges move from transient to
 * retained here. Successful WRITE has no terminals; a failed WRITE returns
 * ALL original terminals so the UI can resume safely. Release the handle after
 * taking it; a result with cancelled=true never transfers terminals. */
bool sleep_io_take(SleepIOJob *job, SleepIOResult *out);
void sleep_io_cancel(SleepIOJob *job);
/* Nonblocking, including an uncollected completed job. A detached owner frees
 * terminals/results and removes unclaimed snapshot files on the worker. */
void sleep_io_release(SleepIOJob *job);
void sleep_io_stats(i32 *jobs, usize *reserved_bytes);

#endif
