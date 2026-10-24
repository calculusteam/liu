/*
 * Liu — agent lifecycle signals lifted off the notify wire.
 *
 * The notify server runs on its own thread inside the GUI (notify_server.h).
 * When a hook frame arrives it carries something the UI could never infer
 * reliably on its own: the agent itself saying "a turn started" / "a turn
 * finished". Output volume cannot tell a long build log from a thinking
 * model, so it cannot stand in for those edges.
 *
 * The server thread pushes; the UI poll drains on the main thread. A tiny
 * mutex-guarded ring is the whole mechanism: signals are rare (a couple per
 * agent turn), so there is nothing here to make lock-free.
 *
 * Overflow drops the OLDEST entry. A stale "started" is worth less than the
 * "finished" that would otherwise be pushed out, and losing a signal only
 * costs an animation frame — never correctness.
 */
#ifndef AGENT_SIGNAL_H
#define AGENT_SIGNAL_H

#include "core/types.h"

#define AGENT_SIGNAL_AGENT_CAP    32
#define AGENT_SIGNAL_CWD_CAP     512
#define AGENT_SIGNAL_SESSION_CAP 128

typedef struct {
    u8   event;                              /* NotifyEvent (EVT_START / EVT_COMPLETE / EVT_NOTIFY) */
    char agent[AGENT_SIGNAL_AGENT_CAP];      /* agent id: "claude", "codex", "grok", … */
    char cwd[AGENT_SIGNAL_CWD_CAP];          /* directory the hook fired in ("" if unknown) */
    char session[AGENT_SIGNAL_SESSION_CAP];  /* agent's own session id ("" if unknown) */
    /* The pane the hook was started in, as that pane's shell exported it in
     * LIU_PANE_ID (`<pid>.<serial>`): the Liu process that owns the pane and
     * the session's serial inside it. Both 0 when the frame carried no pane id
     * or a malformed one — never a guess. The receiver must still compare
     * pane_pid with its own pid: the socket is per user, not per Liu, so a
     * frame from a pane of ANOTHER instance arrives here too. */
    u32  pane_pid;
    u32  pane_serial;
} AgentSignal;

/* Server thread. Copies `sig`; safe to call with a stack value. Calls the wake
 * function below, if one is set, after the signal is in the ring. */
void agent_signal_push(const AgentSignal *sig);

/* Have every push wake whoever drains the ring (the UI passes
 * platform_wake_event_loop). Without it a signal pushed while every pane is
 * quiet sits in the ring until some unrelated event turns the main loop — the
 * tab's "working" bar and the done panel arriving a keystroke late. `wake` runs
 * on the PUSHING thread, after the ring's lock is released, so it must be
 * callable from any thread and may itself take locks; NULL turns it off. */
void agent_signal_set_wake(void (*wake)(void));

/* UI thread. Fills `out` with the oldest pending signal and returns true, or
 * returns false when the ring is empty. Drain in a loop each poll. */
bool agent_signal_pop(AgentSignal *out);

#endif /* AGENT_SIGNAL_H */
