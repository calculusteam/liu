/*
 * A pane that exhausts its read budget can still have bytes buffered.
 * Immediately re-arming its read watch may trigger another wake on macOS
 * EV_DISPATCH and Linux level-triggered polling. The read budget remains
 * nonzero; this module holds the watch for one arming cycle while the main
 * loop continues draining held panes on the next frame. That also keeps
 * sessions sharing a ControlMaster socket moving. A hold is created by session
 * draining and expires at the next arming opportunity on either backend. This
 * table is main-thread-only because socket teardown can run on worker threads.
 */
#ifndef PLATFORM_WATCH_HOLD_H
#define PLATFORM_WATCH_HOLD_H

#include "core/types.h"

/* Largest fd that can carry a hold. On macOS this matches platform_macos.m's
 * WATCH_MAX_FD (2048), which is the largest fd that can carry a WATCH at all.
 * On Linux there is no such ceiling — platform_watch_socket accepts any fd and
 * PLATFORM_MAX_WATCH_FDS caps the COUNT — so an fd at or above this simply
 * cannot be held. That pane keeps today's behaviour: correct, just not
 * backpressured. */
#define WATCH_HOLD_MAX_FD 2048

void watch_hold_set(int fd);
bool watch_hold_is_set(int fd);
i32  watch_hold_pending(void);
/* Main thread only. Clears a hold if present. The platform_unwatch_socket
 * teardown path must not call this function. */
void watch_hold_clear(int fd);

/* LIU_POLL_HOLD=0 disables the mechanism. main.c gates its consumers of
 * AppState.poll_saturated_panes on this too, so the "off" arm is a true revert
 * and the A/B compares against today's behaviour rather than a second
 * modified build. */
bool watch_hold_enabled(void);

/* The arming pass is a state machine. fired indicates whether the backend
 * reported an event since the previous pass. pass_begin returns false only
 * when no work is pending. pass_end expires each hold and records whether a
 * source remains disarmed; that state ensures the next pass runs and re-arms
 * it. */
bool watch_hold_pass_begin(bool fired);
void watch_hold_pass_end(bool skipped_any);

#endif /* PLATFORM_WATCH_HOLD_H */
