/*
 * Liu — "what is this agent doing right now", read off the pane's screen.
 *
 * A chat title from the CLI's own store cannot label a working pane: few of the
 * CLIs Liu recognises publish a live title at all (history/live_title.c), Claude
 * Code hides its placeholder until the model names the conversation, and none of
 * them name it before the first exchange finishes.
 *
 * The agent is already saying what it is doing — it is drawing it. Every CLI
 * covered here paints two lines while a turn runs: a spinner line ("Burrowing…
 * (4s · ↓ 145 tokens)") and, above it, an action line naming the tool call
 * ("Reading lines 200-520 of tab_popup.c"). This module reads them back out of
 * the cells, which is the only mechanism that can work for all of them: it
 * needs no on-disk format, no hook, and no cooperation from the tool.
 *
 * The grammar below is what captures of each real CLI, fed through terminal.c,
 * show:
 *
 *   Claude Code 2.1.245   ✢ Burrowing… (4s · ↓ 145 tokens)
 *                         ⏺ Reading lines 200-520 of tab_popup.c
 *   Codex 0.149.1         • Working (9s • esc to interrupt)
 *                         • Ran 3 commands · ctrl + t to view transcript
 *   OpenCode              ⬝⬝⬝⬝⬝⬝⬝⬝  esc interrupt      tab agents  ctrl+p commands
 *                         → Read src/ui/tab_popup.c
 *   Grok 4.6              ⠧ Responding… 0.9s            9.7s ⇣68.9k [stop]
 *                      ❙  ◈ Read 2 files
 *   Antigravity 1.1.20    ⢿  Reading file...
 *                         ● Read(/Volumes/…/src/ui/tab_popup.c)
 *
 * Three things follow from those five rows:
 *
 *  - The action line is the useful one and the spinner line is the fallback.
 *    "Burrowing…" and "Working" say the agent is alive; "Reading lines 200-520
 *    of tab_popup.c" says what it is doing, which is the question the popup is
 *    being asked. OpenCode's spinner carries no words at all.
 *  - The marker is not always the first glyph on the row: Grok draws a rail
 *    (❙) in front of the item it is currently on, and OpenCode rails its whole
 *    transcript with ┃.
 *  - Codex's spinner line carries an ACTION marker (•), so the marker alone
 *    cannot separate the two. "esc to interrupt" / "[stop]" can, and every CLI
 *    that draws a spinner prints one of those next to it — it has to, or the
 *    user would have no way to stop the turn.
 *
 * Stateless and read-only, like terminal/img_link.c and for the same reason:
 * an agent repaints its live region continuously, so anything that remembered
 * where it last saw a line would be wrong by the next frame.
 *
 * The caller runs it at 2 Hz per AGENT pane (never for a plain shell), and even
 * the worst case — a screen of full-width prose with no marker anywhere, so
 * every row is read to its end and the walk never short-circuits — is cheap at
 * that rate. That is why the status test is applied to every row rather than
 * only to marked ones: it is the only thing that could recognise a CLI that
 * draws its spinner without a glyph.
 */
#ifndef TERMINAL_AGENT_ACTIVITY_H
#define TERMINAL_AGENT_ACTIVITY_H

#include "core/types.h"
#include "terminal/terminal.h"

/* Bytes, including the NUL. Matches PaneAgent.activity. */
#define AGENT_ACTIVITY_MAX 96

/* Scrape the visible screen of `t` for what the agent in it is doing.
 *
 * Returns true and writes a short one-line description into `out` (never
 * longer than `cap - 1` bytes, never split mid-codepoint). Returns false and
 * leaves `out` empty when the screen says nothing usable — a caller that keeps
 * the previous answer on a miss is doing the right thing, since a repaint in
 * progress can momentarily blank the line.
 *
 * Only the LIVE screen is read: a view scrolled into history shows an old turn
 * and would pin a stale answer, so scroll_offset != 0 is a miss.
 */
bool agent_activity_scan(Terminal *t, char *out, usize cap);

/* The line classifier, exposed for tests: given the text of one row (already
 * stripped of its rail and marker), does it read as a spinner/status line?
 * Returns false for an action line. */
bool agent_activity_is_status_text(const char *text);

#endif /* TERMINAL_AGENT_ACTIVITY_H */
