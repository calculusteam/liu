/* crashlog — capture Liu crashes to a `crasherrors/` folder and surface the
 * last one on the next launch.
 *
 * Two capture paths:
 *   - POSIX signals (SIGSEGV/SIGABRT/SIGBUS/SIGILL/SIGFPE/SIGTRAP) via an
 *     async-signal-safe handler that writes a pre-built file path + reason.
 *   - Objective-C uncaught NSExceptions (macOS) via crashlog_record_exception,
 *     called from the platform layer's NSSetUncaughtExceptionHandler — this is
 *     the one that catches things like an invalid window title (the NSException
 *     fires, the reason is recorded, then the default handler aborts).
 *
 * Reports are plain text. The first line is a one-line human reason (it is
 * what the next launch shows); after it come the build (version, commit,
 * architecture), the OS, when the run started, and for a signal the faulting
 * address, the function it crashed in and the crashing thread's backtrace.
 * An Objective-C exception report carries the call stack it was thrown with.
 * Reports are automatically pruned to a fixed cap.
 *
 * The directory is created with every missing parent, and the handler runs on
 * a signal stack of its own (main thread), so the first launch of a fresh
 * install and a stack overflow are both recorded.
 */
#ifndef LIU_CRASHLOG_H
#define LIU_CRASHLOG_H

#include "core/types.h"

/* Install handlers and resolve the crasherrors/ directory (under the Liu config
 * dir). Call once, as early in main() as possible, ON THE MAIN THREAD (the
 * signal stack belongs to the calling thread). Idempotent. */
void crashlog_init(void);

/* Record an uncaught Objective-C exception. Called from the platform NSException
 * handler (normal context — heap/Foundation allowed). `reason` may be NULL. */
void crashlog_record_exception(const char *name, const char *reason,
                               const char *backtrace_text);

/* If a crash report from a PREVIOUS run hasn't been shown yet, copy its one-line
 * reason into `reason_out` and its file path into `path_out`, mark it shown
 * (so it won't surface again), and return true. Returns false when there is
 * nothing pending. Safe to call with cap 0 / NULL outputs (returns existence). */
bool crashlog_take_pending(char *reason_out, usize reason_cap,
                           char *path_out, usize path_cap);

/* Absolute path of the crasherrors/ directory (created on init). */
const char *crashlog_dir(void);

#endif /* LIU_CRASHLOG_H */
