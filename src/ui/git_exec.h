/*
 * Liu — shared `git` subprocess helper.
 *
 * One implementation, in git_status.c, used by both the status sampler and the
 * diff view. It matters that there is exactly one: the argv reaching it comes
 * from a cwd the *terminal* supplied (OSC 7 is attacker-controlled the moment
 * a shell prints a crafted escape), so the safety posture — spawn/exec of an
 * absolute /usr/bin/git with no shell anywhere in the chain, stderr to
 * /dev/null, stdout on a CLOEXEC pipe — has to hold for every caller.
 *
 * Callers run it off the main thread: a cold `git diff` on a large repo takes
 * far longer than a frame.
 */
#ifndef UI_GIT_EXEC_H
#define UI_GIT_EXEC_H

#include "core/types.h"

#include <stdatomic.h>

/* A handle on a git run in progress, so a teardown can stop it.
 *
 * The diff worker's job is a `git diff HEAD` plus a `git ls-files --others`
 * plus one `git diff --no-index` fork PER untracked file (capped, but the cap
 * is in the hundreds). Joining that at window close waited out every one of
 * them, and read()/waitpid() are not interruptible by a flag alone — the
 * child has to be signalled.
 *
 * `cancel` is checked between runs and by the read loop; `child_pgid` is the
 * process GROUP of the run currently in flight (the child calls setpgid, so
 * git's own children go with it and none is orphaned). Zero-initialise it. */
typedef struct {
    _Atomic bool cancel;
    _Atomic int  child_pgid;   /* 0 = nothing running */
} GitCancel;

/* As git_run_capture, but abandons the run when `c` is cancelled — including
 * one already in flight, whose process group is signalled. `c` may be NULL,
 * which makes it exactly git_run_capture. Returns -1 for a cancelled run. */
int git_run_capture_cancellable(const char *cwd, const char *const *argv,
                                char *out, usize out_size, GitCancel *c);

/* Ask the run in flight to stop, and refuse any that has not started. Safe
 * from any thread, and safe to call when nothing is running. SIGTERM first;
 * the reaping side escalates to SIGKILL if the child ignores it. */
void git_cancel_request(GitCancel *c);

/* Run `git <argv...>` with working directory `cwd`, capturing up to
 * `out_size`-1 bytes of stdout into `out` (always NUL-terminated; output past
 * the cap is read and discarded so git never blocks on a full pipe).
 *
 * `argv` must be NULL-terminated and start with "git". Returns git's exit
 * status, or -1 on spawn/pipe failure. */
int git_run_capture(const char *cwd, const char *const *argv,
                    char *out, usize out_size);

/* A capture buffer that grows with the output instead of being sized for the
 * largest output anyone could imagine. `data` is heap (NULL when nothing has
 * been captured), `len` the bytes in it, `cap` what malloc was asked for —
 * the number the memory ledger wants, since a buffer that grew to a peak and
 * was appended to a little is still holding the peak. `data[len]` is always
 * NUL once anything has been captured.
 *
 * `charge`, when set, is called from the CAPTURING thread with the change in
 * `cap` every time the buffer is reallocated or released, so an owner can keep
 * a ledger current without polling a worker. */
typedef void (*GitGrowCharge)(void *ctx, isize cap_delta);
typedef struct {
    char *data;
    usize len;
    usize cap;
    GitGrowCharge charge;
    void *charge_ctx;
} GitGrowBuf;

/* As git_run_capture_cancellable, but APPENDS stdout to `g`, growing it as the
 * bytes arrive, up to `limit` bytes of THIS run's output (the run's own bytes,
 * not the buffer's total). Output past the limit is read and discarded, the
 * way the fixed-size capture does, so git never blocks on a full pipe; the
 * caller can tell it happened because the run's appended length equals
 * `limit`. Growth is geometric, so a 73-byte diff costs a few hundred bytes
 * and a large one is not copied per read. Returns git's exit status, -1 on
 * spawn/pipe failure or cancel, and -2 when the buffer could not be grown (the
 * bytes captured so far are kept). */
int git_run_capture_grow(const char *cwd, const char *const *argv,
                         GitGrowBuf *g, usize limit, GitCancel *c);

/* Make room for at least `need` more bytes plus the terminator. Returns false
 * (leaving `g` untouched) when the allocation fails. */
bool git_grow_reserve(GitGrowBuf *g, usize need);
/* Shrink `cap` to fit `len` (plus the terminator). A buffer that grew by
 * doubling can hold nearly twice its content; once the run is over that is
 * the process holding capacity nobody will use. */
void git_grow_trim(GitGrowBuf *g);
/* Release the buffer and charge the release. Leaves `g` empty and reusable. */
void git_grow_free(GitGrowBuf *g);

#endif /* UI_GIT_EXEC_H */
