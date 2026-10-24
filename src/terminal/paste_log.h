/*
 * Liu — the text behind an agent's "[Pasted text …]", put back on copy.
 *
 * Every agent CLI that takes a bracketed paste folds a large one into a
 * placeholder in its prompt box, and the placeholder is all that is ever on
 * the screen. Selecting it and pressing Cmd+C would copy the placeholder — the
 * words "[Pasted text #1 +11 lines]" — when the user wants what is BEHIND it.
 * The agent keeps that text in its own memory, where no terminal can read it;
 * but Liu typed it, so Liu can remember it. This module is that memory, per
 * pane, and the rule that puts it back into a copy.
 *
 * The placeholders each CLI draws, for pastes of 12 lines/293 chars, one line
 * of 1500, 3 short lines, 40 lines + trailing newline, and 1200 x U+015F:
 *
 *   Claude Code 2.1.282   [Pasted text #N +L lines]   L = line breaks (11, 40)
 *                         [Pasted text #N]            one line
 *   Codex 0.156.1         [Pasted Content C chars]    C = characters, not bytes
 *   OpenCode              [Pasted ~L lines]           L = 12, 1, 3, 40
 *   Antigravity (agy)     [Pasted text #N +L lines]   L = line breaks (40)
 *                         [Pasted text #N C chars]    one line
 *   Kimi                  [Pasted text #N +L lines]   L = line breaks + 1 (41)
 *                         [Pasted text #N]            one line
 *   Command Code 1.56     [e-line-01… +40L]           first 10 chars + lines
 *   Cline CLI             never enables bracketed paste — nothing to fold
 *
 * The thresholds differ (Codex and Antigravity left 12 lines unfolded, Claude
 * folded them; nobody folded 3 short lines) and nobody prints the text itself,
 * so a placeholder is recognised by its SHAPE and trusted only when its
 * numbers agree with a paste this pane actually received: a line count may be
 * the break count or one more (the two conventions above), a character count
 * must be exact, Command Code's preview must be the paste's own prefix, and a
 * bare "[Pasted text #N]" only ever stands for a one-line paste. The "#N" is
 * never used — each CLI numbers per prompt or per session, and nothing Liu
 * sees says which. A placeholder whose numbers match nothing is left as it
 * was, and its words are copied.
 *
 * Several placeholders in one selection map to pastes in the same ORDER, each
 * taking the newest paste that fits and is older than the one after it — the
 * transcript lists a prompt's pastes in the order they were made.
 *
 * The one ambiguity left: a bare "[Pasted text #N]" (Claude, Kimi) has no
 * count to check, so copied ON ITS OWN it takes the newest one-line paste of
 * this pane. Selected together with the placeholders after it, the order rule
 * places it correctly; an old one-liner copied alone after a newer one-line
 * paste gets the newer text. Every other shape carries a count or a head that
 * must match exactly.
 *
 * Owned by the pane's Terminal (`Terminal.paste_log`, freed through
 * `Terminal.paste_log_free` so terminal.c links without this file), main
 * thread only, charged to MEM_TERMINAL. Bounded: PASTE_LOG_MAX_ENTRIES pastes,
 * PASTE_LOG_MAX_BYTES in total, oldest dropped first; a paste bigger than
 * PASTE_LOG_MAX_ONE is not remembered at all.
 */
#ifndef TERMINAL_PASTE_LOG_H
#define TERMINAL_PASTE_LOG_H

#include "core/types.h"

typedef struct Terminal Terminal;
typedef struct PasteLog PasteLog;

#define PASTE_LOG_MAX_ENTRIES 64
#define PASTE_LOG_MAX_BYTES   ((usize)32 * 1024 * 1024)
#define PASTE_LOG_MAX_ONE     ((usize)16 * 1024 * 1024)
/* A paste smaller than this with no line break is typed-sized: no CLI folds
 * it (the shortest one-line paste folded in the table above is 1200
 * characters), so it is not worth a slot that a real block could use. */
#define PASTE_LOG_MIN_ONE_LINE 200

PasteLog *paste_log_create(void);
void      paste_log_destroy(PasteLog *log);
/* Remember `len` bytes exactly as the program received them (terminators
 * already filtered). Returns false when the paste was not kept (too small,
 * too large, or out of memory). */
bool      paste_log_add(PasteLog *log, const char *text, usize len);
/* `sel` with every placeholder that matches a remembered paste replaced by the
 * paste, as a new heap string; NULL when nothing matched (the caller keeps
 * `sel`). `*n_expanded` (optional) receives the count. */
char     *paste_log_expand(const PasteLog *log, const char *sel, i32 *n_expanded);
i32       paste_log_count(const PasteLog *log);
usize     paste_log_bytes(const PasteLog *log);

/* The pane-level glue: the log hangs off the pane's Terminal and is created
 * on its first paste. */
void      paste_log_record(Terminal *t, const char *text, usize len);
char     *paste_log_expand_terminal(const Terminal *t, const char *sel, i32 *n_expanded);

#endif
