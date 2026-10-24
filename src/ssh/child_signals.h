/*
 * Liu — the signals a program started in a pane begins with
 *
 * An ignored disposition and a blocked signal both survive execve, and a shell
 * hands them on to every program it runs. Started with SIGINT ignored, a
 * pane's foreground job would never die of Ctrl+C: the line discipline sends
 * the signal and the job has been told to ignore it. Liu ignores SIGPIPE
 * itself (the notify server), and it inherits whatever its own launcher left
 * behind: a non-interactive shell runs `Liu &` — an .xinitrc, a session script
 * — with SIGINT and SIGQUIT ignored, and nohup ignores SIGHUP.
 *
 * So in the child, between fork and exec, every signal a terminal's programs
 * expect at its default is put back there and nothing stays blocked, whatever
 * Liu was started with. Async-signal-safe calls only (the parent is
 * multithreaded).
 */
#ifndef LIU_SSH_CHILD_SIGNALS_H
#define LIU_SSH_CHILD_SIGNALS_H

#include "core/types.h"

#include <signal.h>

static inline void child_signals_default(void) {
    static const int sigs[] = {
        SIGHUP, SIGINT, SIGQUIT, SIGPIPE, SIGALRM, SIGTERM, SIGCHLD,
        SIGTSTP, SIGTTIN, SIGTTOU, SIGUSR1, SIGUSR2, SIGWINCH,
    };
    struct sigaction sa;
    sa.sa_handler = SIG_DFL;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    for (usize i = 0; i < sizeof sigs / sizeof sigs[0]; i++)
        sigaction(sigs[i], &sa, NULL);
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, NULL);
}

#endif /* LIU_SSH_CHILD_SIGNALS_H */
