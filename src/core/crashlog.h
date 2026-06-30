/* crashlog — capture Liu crashes to a `crasherrors/` folder and surface the
 * last one on the next launch.
 *
 * Two capture paths:
 *   - POSIX signals (SIGSEGV/SIGABRT/SIGBUS/SIGILL/SIGFPE/SIGTRAP) via an
 *     async-signal-safe handler that writes a pre-built file path + backtrace.
 *   - Objective-C uncaught NSExceptions (macOS) via crashlog_record_exception,
 *     called from the platform layer's NSSetUncaughtExceptionHandler — this is
 *     the one that catches things like an invalid window title (the NSException
 *     fires, we record the reason, then the default handler aborts).
 *
 * Reports are plain text. The first line is a one-line human reason; the rest
 * is detail (backtrace / call stack). The folder is never auto-pruned beyond a
 * cap so a user can inspect history.
 */
#ifndef LIU_CRASHLOG_H
#define LIU_CRASHLOG_H

#include "core/types.h"

/* Install handlers and resolve the crasherrors/ directory (under the Liu config
 * dir). Call once, as early in main() as possible. Idempotent. */
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
