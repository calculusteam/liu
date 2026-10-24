/*
 * Liu — when the app tells the OS it is at work, so that it is not App-Napped.
 *
 * macOS naps an app whose windows are all covered: after about a minute every
 * thread of the process, the pty readers included, runs at priority 4, whatever
 * QoS it asked for. With nothing else going on that costs nothing. With the
 * machine busy it stops the program in the pane: the kernel's pty queue is 1024
 * bytes, the writer sleeps until the reader has taken them, and a reader at
 * priority 4 gets a turn only a few times a second.
 *
 * An agent CLI writes to its terminal synchronously, so that is the AGENT standing
 * still, not only its picture — while the user is in another app, which is when
 * agents are left to work.
 *
 * So while something is at work in a pane, Liu holds an activity
 * (platform_set_user_activity; the platform file has which one, and why the
 * obvious one is not enough — it still leaves every thread at priority 20). "At
 * work" is: a job owns a local pane's terminal, a remote link is up, or output
 * arrived since the last look — the last one is what covers a job left running
 * behind the prompt, without asking the kernel for the pane's process list every
 * second. A Liu whose panes all sit at a quiet prompt holds nothing and may be
 * napped.
 *
 * Pure, so it can be tested without a window.
 */
#ifndef CORE_ACTIVITY_HOLD_H
#define CORE_ACTIVITY_HOLD_H

#include "core/types.h"

/* How long after the last sign of work the activity is still held: a build
 * that prints nothing for a while is still a build, and taking and dropping
 * the activity around every prompt would be noise for the system. */
#define ACTIVITY_HOLD_LINGER_SEC 30.0
/* How often the panes are looked at. */
#define ACTIVITY_HOLD_LOOK_SEC    1.0

typedef struct {
    f64  busy_until;        /* held while now < busy_until */
    f64  next_look;
    bool held;
} ActivityHold;

/* Is it time to look at the panes again? True at most once per look period. */
static inline bool activity_hold_due(ActivityHold *h, f64 now) {
    if (now < h->next_look) return false;
    h->next_look = now + ACTIVITY_HOLD_LOOK_SEC;
    return true;
}

/* `busy`: something is at work right now. Returns whether to hold. */
static inline bool activity_hold_step(ActivityHold *h, bool busy, f64 now) {
    if (busy) h->busy_until = now + ACTIVITY_HOLD_LINGER_SEC;
    h->held = now < h->busy_until;
    return h->held;
}

/* One pane.
 *   local:  a job owns the terminal when the foreground process group is not
 *           the shell's own (ui/sleep_policy.h has the long form). A shell
 *           there but a foreground that could not be read counts as work: the
 *           cost of being wrong is one un-napped app.
 *   remote: nothing local to ask, and the far side's output is drained by
 *           this process — a link that is up is work.
 * `link_up` is "connected or connecting"; fg_pgid / shell_pgid are -1 when
 * they do not apply. */
static inline bool activity_pane_at_work(bool local, bool link_up, i32 fg_pgid, i32 shell_pgid) {
    if (!link_up) return false;
    if (!local) return true;
    if (shell_pgid <= 0) return false;      /* no child of ours on this pane */
    return fg_pgid != shell_pgid;           /* a job, or unreadable */
}

#endif /* CORE_ACTIVITY_HOLD_H */
