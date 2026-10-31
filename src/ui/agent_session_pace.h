/*
 * Liu — how often a pane looks up the stored session its agent is writing
 * (history/agent_session.h).
 *
 * While no session is known the look is frequent for a minute — most agents
 * open their session with the first message, which tends to come soon after
 * the agent starts — and then slow, so an agent left waiting for input does
 * not have its whole store read every few seconds for hours. A prompt sent
 * into the pane brings the next look close and makes the looks frequent for
 * another minute. Once a session is known, a slow look still follows a
 * /resume or a new chat inside the agent; each look that finds the same
 * session again doubles the wait, up to AGENT_SESSION_KNOWN_MAX_SEC, and a
 * prompt sent into the pane starts it over — a /resume or a new chat is
 * followed by a prompt, and the look after that prompt finds it.
 */
#ifndef UI_AGENT_SESSION_PACE_H
#define UI_AGENT_SESSION_PACE_H

#include "core/types.h"

#define AGENT_SESSION_FIRST_LOOK_SEC    3.0   /* after the agent appears */
#define AGENT_SESSION_EAGER_SEC         5.0   /* none known, frequent */
#define AGENT_SESSION_EAGER_FOR_SEC    60.0   /* how long "frequent" lasts */
#define AGENT_SESSION_IDLE_SEC         30.0   /* none known, after that */
#define AGENT_SESSION_AFTER_PROMPT_SEC  2.0   /* a prompt was just sent */
#define AGENT_SESSION_KNOWN_FRONT_SEC  20.0   /* known, the active tab */
#define AGENT_SESSION_KNOWN_BACK_SEC   45.0   /* known, a background tab */
#define AGENT_SESSION_KNOWN_MAX_SEC   300.0   /* known, and found again and again */

/* Seconds until the next look. `known`: the pane has its session; `front`:
 * its tab is the active one; `eager_until`: the loop clock until which an
 * unknown session is looked for frequently; `same_looks`: how many looks in a
 * row found the known session unchanged. */
static inline f64 agent_session_look_every(bool known, bool front, f64 now, f64 eager_until,
                                           u32 same_looks) {
    if (known) {
        f64 every = front ? AGENT_SESSION_KNOWN_FRONT_SEC : AGENT_SESSION_KNOWN_BACK_SEC;
        for (u32 i = 0; i < same_looks && every < AGENT_SESSION_KNOWN_MAX_SEC; i++) every *= 2.0;
        return every < AGENT_SESSION_KNOWN_MAX_SEC ? every : AGENT_SESSION_KNOWN_MAX_SEC;
    }
    return now < eager_until ? AGENT_SESSION_EAGER_SEC : AGENT_SESSION_IDLE_SEC;
}

/* When the next look happens after a prompt sent at `now`: no later than
 * AGENT_SESSION_AFTER_PROMPT_SEC from now. A look due sooner keeps its time;
 * `next` 0 is a pane whose first look is not scheduled yet. */
static inline f64 agent_session_look_after_prompt(f64 next, f64 now) {
    f64 soon = now + AGENT_SESSION_AFTER_PROMPT_SEC;
    return (next <= 0.0 || next > soon) ? soon : next;
}

/* The same for a pane whose session is known: the next look no later than the
 * usual wait from now (the caller starts `same_looks` over). `next` 0 — no
 * look scheduled yet — stays. */
static inline f64 agent_session_known_look_after_prompt(f64 next, f64 now) {
    f64 soon = now + AGENT_SESSION_KNOWN_FRONT_SEC;
    return (next > soon) ? soon : next;
}

#endif /* UI_AGENT_SESSION_PACE_H */
