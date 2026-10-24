/*
 * Close a pane after its local shell exits normally; close the tab too when it
 * was the only pane.
 *
 * If the shell exits during startup, keep the pane open with its output and
 * close it on Enter. This preserves startup errors for inspection and prevents
 * a failed shell from being replaced repeatedly.
 *
 * The session poll reports the state; `ui.c` and `main.c` perform the close.
 */
#ifndef UI_SHELL_EXIT_H
#define UI_SHELL_EXIT_H

#include "core/types.h"

#include <stdio.h>

typedef enum {
    SHELL_EXIT_CLOSE,   /* close the pane, or the tab when it is the only pane */
    SHELL_EXIT_HOLD,    /* keep the pane, say what happened, wait for Enter */
} ShellExitAction;

/* Treat an exit within `SHELL_EXIT_EARLY_SEC` as a startup failure, such as
 * an rc file exit, missing interpreter, or failed exec. A later exit closes
 * normally. */
#define SHELL_EXIT_EARLY_SEC 1.0

/* `lived_sec` is session_lived_sec(): negative means "not known", which is
 * treated as early — an unexplained close is the outcome to avoid. */
static inline ShellExitAction shell_exit_decide(f64 lived_sec) {
    return (lived_sec >= SHELL_EXIT_EARLY_SEC) ? SHELL_EXIT_CLOSE : SHELL_EXIT_HOLD;
}

/* The line a held pane shows, as bytes for its terminal. `code` is
 * session_exited()'s: an exit status, or the negated signal number.
 * `leave_alt` asks for the alternate screen to be left first (a full-screen
 * program that died there would otherwise hide the line when it is redrawn).
 * Returns the length written, 0 when it does not fit. */
static inline i32 shell_exit_notice(char *out, usize cap, int code, bool leave_alt) {
    char what[64];
    if (code < 0)       snprintf(what, sizeof what, "killed by signal %d", -code);
    else if (code == 0) snprintf(what, sizeof what, "exit status 0");
    else                snprintf(what, sizeof what, "exit status %d", code);
    int n = snprintf(out, cap,
                     "%s\x1b[?25h\x1b[0m\r\n"
                     "\x1b[2m[the shell exited as soon as it started (%s) - press Enter to close]\x1b[0m\r\n",
                     leave_alt ? "\x1b[?1049l" : "", what);
    if (n <= 0 || (usize)n >= cap) { if (cap) out[0] = '\0'; return 0; }
    return (i32)n;
}

#endif /* UI_SHELL_EXIT_H */
