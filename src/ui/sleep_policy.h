/*
 * Sleeping a tab serializes its scrollback and destroys its `Terminal`, then
 * sends `SIGSTOP` to the shell's process group. Interactive shell jobs use
 * separate process groups, so they may continue writing to the PTY if only the
 * shell group is stopped and eventually block on a full buffer.
 *
 * A pane is idle only when the shell is the PTY's foreground process group and
 * no other process remains on that PTY. The foreground check alone would miss
 * background jobs. Keep this policy separate from the ioctls that supply its
 * inputs.
 */
#ifndef UI_SLEEP_POLICY_H
#define UI_SLEEP_POLICY_H

#include "core/types.h"

/* When shell_pgid <= 0, there is no local child to protect, so this input does
 * not block sleep; the caller's session-type rules still apply. When fg_pgid
 * <= 0, a shell exists but its foreground process group could not be queried,
 * so sleep is refused because a running job may still need the terminal. */
/* bg_jobs:    live processes sharing this pane's controlling terminal that are
 *             OUTSIDE the shell's own process group -- the jobs the shell has
 *             left running behind its prompt. -1 when the query itself failed.
 *             See session_background_jobs().
 *
 * Why this is a third input and not a detail of the second. `fg_pgid ==
 * shell_pgid` says the shell has the terminal, which is the same thing as "the
 * shell is at its prompt" -- and a shell at its prompt may still have `npm run
 * dev &` running behind it. session_suspend() signals the SHELL's process
 * group, and an interactive shell puts each job in its OWN group, so that job
 * is not stopped by the sleep. It keeps running and keeps writing into a pty
 * nobody drains any more, wedges in write() as soon as whatever sits behind
 * the pty fills (the reader ring, or with LIU_PTY_THREAD=0 only the kernel tty
 * queue), and stays wedged until the tab is woken. */
typedef enum {
    SLEEP_PANE_IDLE = 0,     /* shell at its prompt, nothing else on the tty */
    SLEEP_PANE_FOREGROUND,   /* a job owns the terminal                      */
    SLEEP_PANE_BACKGROUND,   /* prompt is back, but jobs run behind it       */
    SLEEP_PANE_UNKNOWN,      /* a query failed -- treated as busy            */
} SleepPaneState;

static inline SleepPaneState sleep_pane_state(i32 fg_pgid, i32 shell_pgid,
                                              i32 bg_jobs) {
    /* No local child to protect at all (a remote session, or one that never
     * started one). Nothing can be wedged, so neither of the other two inputs
     * is a reason to refuse -- and neither is meaningful for that pane. */
    if (shell_pgid <= 0) return SLEEP_PANE_IDLE;
    /* There IS a shell, but what is in the foreground could not be read. */
    if (fg_pgid <= 0)    return SLEEP_PANE_UNKNOWN;
    if (fg_pgid != shell_pgid) return SLEEP_PANE_FOREGROUND;
    /* The prompt is back. Fail CLOSED on a failed background query, for the
     * same reason the foreground unknown does: being wrong costs a hung dev
     * server, being conservative costs one tab's memory. */
    if (bg_jobs < 0)  return SLEEP_PANE_UNKNOWN;
    if (bg_jobs > 0)  return SLEEP_PANE_BACKGROUND;
    return SLEEP_PANE_IDLE;
}

/* The two-input form, kept because "is a job in the FOREGROUND" is still a
 * question on its own and is what the cheap pair of ioctls answers. A caller
 * that can also count background jobs must use sleep_pane_state() -- this one
 * cannot see them and answers idle. */
static inline bool sleep_pane_is_idle(i32 fg_pgid, i32 shell_pgid) {
    return sleep_pane_state(fg_pgid, shell_pgid, 0) == SLEEP_PANE_IDLE;
}

/* Sleeping destroys the pane's Terminal and calls session_suspend().
 * Eligibility depends on the session type:
 *
 * LOCAL: killpg(SIGSTOP) stops the shell process group. Sleep is allowed only
 * after the session scan finds no separate background job group; foreground
 * equality alone does not prove that the pane is idle.
 *
 * SSH: the remote shell continues running while suspended. The poll loop drains
 * channel output into a scratch buffer, so those bytes are discarded; sleep
 * remains blocked until output can be preserved.
 *
 * MOSH: killpg(SIGSTOP) stops the local mosh process. On resume, state
 * synchronization restores the current screen, but mosh does not replay
 * scrollback produced during suspension.
 *
 * TELNET and SERIAL: there is no suspendable process, and TabSleepSnapshot
 * stores only SSHConfig, so these sessions cannot be restored.
 *
 * Keep the session kinds local to avoid an SSH/libssh2 dependency. The switch
 * in ui.c has no default, so a new session type produces a compiler warning. */
typedef enum {
    SLEEP_KIND_LOCAL = 0,
    SLEEP_KIND_SSH,
    SLEEP_KIND_MOSH,
    SLEEP_KIND_TELNET,
    SLEEP_KIND_SERIAL,
    SLEEP_KIND_NONE,      /* no session in this pane — nothing to lose */
} SleepSessionKind;

/* May a pane of this kind be slept at all? Independent of whether it is busy,
 * collapsed or explicitly requested: those are later questions, and none of
 * them can make a discarded remote stream come back. */
static inline bool sleep_kind_may_sleep(SleepSessionKind k) {
    switch (k) {
        case SLEEP_KIND_NONE:   return true;
        case SLEEP_KIND_LOCAL:  return true;
        case SLEEP_KIND_MOSH:   return true;
        case SLEEP_KIND_SSH:    return false;
        case SLEEP_KIND_TELNET: return false;
        case SLEEP_KIND_SERIAL: return false;
    }
    return false;   /* an unmapped kind is refused, not assumed harmless */
}

/* An explicit sleep request bypasses auto-sleep-disabled and collapsed-group
 * blocks, but session-kind and busy blocks still apply. */
typedef enum {
    SLEEP_OK = 0,
    SLEEP_BLOCK_KIND,        /* telnet / serial — no suspendable process */
    SLEEP_BLOCK_REMOTE,      /* SSH — output produced while asleep is dropped */
    SLEEP_BLOCK_BUSY,        /* a job holds the terminal in some pane */
    SLEEP_BLOCK_BGJOB,       /* a job runs BEHIND the prompt in some pane */
    SLEEP_BLOCK_COLLAPSED,   /* the tab's group is collapsed */
    SLEEP_BLOCK_DISABLED,    /* auto-sleep turned off for this tab */
} SleepBlock;

/* One human sentence per reason, for the toast. */
static inline const char *sleep_block_reason(SleepBlock b) {
    switch (b) {
        case SLEEP_BLOCK_KIND:      return "This session type cannot be slept";
        case SLEEP_BLOCK_REMOTE:    return "Sleeping would discard this session's output";
        case SLEEP_BLOCK_BUSY:      return "A command is still running in this tab";
        case SLEEP_BLOCK_BGJOB:     return "A background job is still running in this tab";
        case SLEEP_BLOCK_COLLAPSED: return "Tabs in a collapsed group are kept awake";
        case SLEEP_BLOCK_DISABLED:  return "Auto-sleep is disabled for this tab";
        default:                    return "";
    }
}

/* A time budget alone does not bound snapshot writes. A full scrollback can
 * write several megabytes, with most of the cost in filesystem operations, so
 * SLEEP_SNAP_SCAN_BUDGET_BYTES also limits the bytes committed in a pass.
 * Deferred tabs are already idle, so the next 30-second tick can handle them.
 *
 * The first tab is admitted even if its estimated snapshot exceeds the byte
 * budget. Otherwise one oversized snapshot could never be written. A pass can
 * therefore exceed the budget by at most one snapshot. */

/* A compressed scrollback line costs a little over 3 bytes per column. Four is
 * the round number ABOVE that — an over-estimate stops the pass sooner, which
 * is the safe direction for a budget whose job is to bound work. */
#define SLEEP_SNAP_BYTES_PER_COL_LINE 4u
#define SLEEP_SNAP_SCAN_BUDGET_BYTES  ((usize)4u * 1024u * 1024u)

/* What one pane's snapshot will cost: the compressed scrollback plus the
 * primary screen, which sleep_snap_write emits as a flat Cell stream. */
static inline usize sleep_snap_estimate_bytes(i32 sb_count, i32 cols, i32 rows,
                                              usize cell_size) {
    if (cols <= 0) return 0;
    usize c = (usize)cols;
    usize sb = sb_count > 0 ? (usize)sb_count * c * SLEEP_SNAP_BYTES_PER_COL_LINE : 0;
    usize screen = rows > 0 ? (usize)rows * c * cell_size : 0;
    return sb + screen;
}

/* May a pass that has already committed `written` bytes take on `next`?
 * `written == 0` is the first tab of the pass and is always allowed. */
static inline bool sleep_scan_budget_allows(usize written, usize next) {
    if (written == 0) return true;
    if (next > SLEEP_SNAP_SCAN_BUDGET_BYTES) return false;
    return written + next <= SLEEP_SNAP_SCAN_BUDGET_BYTES;
}

#endif /* UI_SLEEP_POLICY_H */
