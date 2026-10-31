/*
 * Liu — a pane's screen and history as the bytes that draw them again.
 *
 * When Liu reopens its tabs, a pane can show what it showed before (the
 * `restore_screen` setting): the primary screen and its scrollback are kept
 * between launches as a byte stream — text, its colours and styles as SGR,
 * one line break per hard line — and fed to the new pane's terminal before
 * its shell prints anything.
 *
 * A stream and not a copy of the grid: it is read by whatever build comes
 * next, the new terminal can be another width (a soft-wrapped line is written
 * whole and wraps again at the new width), and it can hold nothing a terminal
 * acts on beyond drawing text — no query, no mode, no title, no link. On the
 * alternate screen (an agent's full-screen UI) the stream is the primary
 * screen parked behind it, which is what the shell left; the agent draws its
 * own screen again when it is resumed.
 */
#ifndef TERMINAL_SCREEN_REPLAY_H
#define TERMINAL_SCREEN_REPLAY_H

#include "core/types.h"
#include "terminal/terminal.h"

/* The most rows of history a stream starts from, and its byte budget: the
 * oldest lines go first when either would be exceeded. */
#define SCREEN_REPLAY_MAX_ROWS   4000
#define SCREEN_REPLAY_MAX_BYTES  (4u * 1024u * 1024u)

/* Builds the stream for `t` into a malloc'd buffer (`*out`, `*out_len`; the
 * caller frees it). An empty screen is a true with *out_len 0. False only
 * when memory ran out. */
bool screen_replay_build(Terminal *t, u8 **out, usize *out_len);

/* True when `data` holds nothing but what screen_replay_build writes:
 * printable UTF-8, CR, LF, and SGR sequences (`ESC [ <digits ; :> m`). A
 * stream read back from disk is fed to a terminal only after this says so. */
bool screen_replay_is_inert(const u8 *data, usize len);

#endif /* TERMINAL_SCREEN_REPLAY_H */
