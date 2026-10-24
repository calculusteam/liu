/*
 * Liu — shell command-line geometry.
 *
 * Recalling a command from history into the prompt, and Translate replacing
 * the prompt an agent is showing, need the same answer: where on screen does
 * the *typed* part of the current prompt line start and end? The shell owns
 * that text — liu only sees cells — so the span is recovered from the grid,
 * best evidence first:
 *
 *   1. liu's live mirror of what was typed at this prompt (the autosuggest
 *      shadow), when it still matches the cells ending at the cursor,
 *   2. OSC 133 shell-integration markers (prompt end = input start),
 *   3. a prompt-sigil scan of the logical line ("… % ", "… $ ", "❯ ", …),
 *   4. the whole logical line.
 *
 * The same ladder answers it for an agent CLI's drawn input area, which is a
 * different shape entirely and is selected by the `agent_ui` argument (or
 * detected from the terminal's modes). There the input is a *block* of rows the
 * app painted itself — the soft-wrap chain says nothing about it — so the block
 * is walked out from the cursor row and bounded by its own body columns:
 *
 *     ────────────────────────────────      ┃  hello world, this is a long
 *     ❯ hello world, this is a long         ┃  text that keeps going
 *       text that keeps going               ┃
 *     ────────────────────────────────      ┃  Build · model · max
 *
 * Rules of the block: a row of borders or blanks ends it, a prompt sigil starts
 * it, borders and the continuation indent are padding rather than typed text,
 * and each row's trailing padding is dropped except for the single blank that
 * stands in for the space the app wrapped on. Getting that blank right is what
 * makes the character count equal the number of Backspaces the app needs.
 *
 * Positions are screen cells (visible rows, scroll_offset == 0 only) and
 * offsets are *characters*: a wide CJK glyph is one character occupying two
 * cells, which is also how the shell's cursor counts them, so an offset
 * difference is exactly the number of arrow keys or backspaces to send.
 */
#ifndef TERMINAL_CMDLINE_H
#define TERMINAL_CMDLINE_H

#include "core/types.h"
#include "terminal/terminal.h"

typedef struct {
    i32 start_row, start_col;  /* first cell of the typed text (inclusive) */
    i32 end_row,   end_col;    /* one past the last cell (exclusive)       */
    i32 nchars;                /* characters in the span                    */
    i32 cursor_off;            /* terminal cursor, in chars from the start  */

    /* Body columns of a drawn input block, [body_left, body_right). Outside
     * them sit the app's borders and its continuation indent, which are not
     * typed text; for a shell line they are the full width. */
    i32 body_left, body_right;
    /* Multi-row block geometry is in force: rows are joined at their trimmed
     * ends rather than at the screen edge. False for an ordinary shell line,
     * whose soft-wrap continues at the very edge of the row. */
    bool boxed;
} CmdSpan;

/* Locate the current input line. `typed_hint` is liu's live typed mirror for
 * this terminal (may be NULL); `agent_ui` forces the drawn-block geometry for
 * a pane known to be running an agent CLI, which is also inferred from the
 * terminal's own modes. Returns false on scrollback or when the line yields no
 * usable span. */
bool cmdline_span(Terminal *t, const char *typed_hint, bool agent_ui,
                  CmdSpan *out);

/* The whole drawn input `sp` is a part of. cmdline_span() walks out from the
 * cursor's row while rows carry text, so a prompt with an empty line in it is
 * several spans; this joins them — up to the row the prompt's sigil is on, and
 * down to the last text above the frame that closes the input, where there is
 * one. Rows and body columns only: nchars and cursor_off stay the block's. */
void cmdline_span_whole(Terminal *t, CmdSpan *sp);

/* UTF-8 text of the character range [from, to). Caller frees. */
char *cmdline_text(Terminal *t, const CmdSpan *sp, i32 from, i32 to);

/* Conservative capture for asynchronous prompt replacement. Requires a
 * visible, bounded agent input containing the terminal cursor.
 *
 * An input on ONE row is its cells. One on several rows is not: cells cannot
 * tell the space an app wrapped on from a line break, nor from nothing at all
 * (a word longer than the row is cut at its end), so the rows after the first
 * must read as `mirror` — liu's copy of what was typed here — does, and the
 * mirror supplies what stands between them; an empty row inside the input is
 * a line break of the mirror's. The mirror need not reach the start of the
 * input: what it does not cover is taken from the cells as long as it sits on
 * the first row (an attachment chip, a folded paste, text typed before the
 * mirror was lost). Always the WHOLE prompt, never a part of it.
 *
 * `mirror_sure`: nothing has been pressed that the mirror could not follow.
 * Only then is it believed about what no cell shows — a blank typed last that
 * the caret has since been taken back over, a blank before a line break, a
 * line break typed last, and the top of a prompt taller than its app draws.
 *
 * `app`: what is known of the program that draws the input (CmdInputApp).
 *
 * `*typed_from` (may be NULL) is the byte of `out` from which the text is the
 * mirror's, strlen(out) when none of it is: a "[Image #1]" before it is the
 * app's attachment, one after it was typed letter by letter. */
typedef struct {
    /* An input of up to this many rows is on screen whole (0 or 1 when
     * nothing is known). Taller, it may be cut from the top with nothing to
     * show for it, and its first row on screen is then not the prompt's first:
     * only a mirror that is sure, and covers what is on screen, says what the
     * prompt is. */
    i32  whole_rows;
    /* Nothing but the prompt's own rows stands between its text and the frame
     * that closes it: an empty row there is a line break typed last (or the
     * caret under a row that is full), and has to be accounted for. */
    bool tight_frame;
    /* Its rows are wrapped at a word, `wrap_width` columns from where the text
     * begins, and a word longer than that is cut where the row ends (0 when the
     * rule is not known). With that known, an input that is on screen whole can
     * be READ from its cells: what joins two rows is decided by whether the
     * next row's first word would have fitted (cmdline_capture). */
    i32  wrap_width;
    /* What the same box opens with in ANOTHER mode, where it is not a prompt
     * (Claude Code: "!" first turns it into a shell command's, and the "!"
     * takes the sigil's place). 0 = it has no such mode. */
    u32  other_sigil;
    /* An empty input shows a hint, painted dim; what is typed never is. */
    bool dim_hint;
    /* The app paints text that is not typed on the input's row: a hint far to
     * the right of an empty input and a completion that starts AT the caret,
     * both in a colour other than the typed text's (Oh My Pi). It is not part
     * of the prompt; cmdline_span_app() leaves it out. */
    bool ghost;
} CmdInputApp;

/* cmdline_span() for an input of a known app: `app->ghost` leaves out what the
 * app paints that was not typed. */
bool cmdline_span_app(Terminal *t, const char *typed_hint, bool agent_ui,
                      const CmdInputApp *app, CmdSpan *out);

bool cmdline_capture_prompt_ex(Terminal *t, const char *mirror, bool mirror_sure,
                               CmdInputApp app, char *out, usize cap, usize *typed_from);

/* The capture with its answer spelled out.
 *
 *   EXACT  every character is vouched for, and their count: what
 *          cmdline_capture_prompt_ex() returns true for. An erase may be
 *          counted from it.
 *   WHOLE  the prompt as its cells show it, the rows joined by the app's own
 *          wrap rule (CmdInputApp.wrap_width). It is all of the prompt — the
 *          input is on screen whole — but blanks that are in no cell are not
 *          in it, so nothing may be COUNTED from it: it is good for an erase
 *          that empties the input, and for nothing else. It needs no mirror,
 *          which is its point: a prompt recalled from the agent's history,
 *          dictated by the agent itself, or edited with keys no mirror can
 *          follow is read all the same.
 *
 * `mirror_caret` is the mirror's caret as a byte offset (-1 when not known):
 * a mirror that says its caret is at the end, while the cursor shows fewer
 * blanks after the text than it holds, is not the prompt — Claude Code
 * swallows the blanks of a held space bar — and is not believed about what
 * no cell shows.
 *
 * A refusal says why, for the notice. */
typedef enum {
    CMD_CAPTURE_NONE = 0,
    CMD_CAPTURE_EXACT,
    CMD_CAPTURE_WHOLE,
} CmdCaptureKind;

typedef enum {
    CMD_REFUSED_NOT = 0,
    CMD_REFUSED_EMPTY,       /* nothing is typed (or only the app's hint is shown) */
    CMD_REFUSED_NO_INPUT,    /* no prompt with the caret in it: scrolled, another mode, a menu */
    CMD_REFUSED_CUT,         /* taller than the app draws, and no mirror knows the top */
    CMD_REFUSED_UNREAD,      /* on several rows, and neither a mirror nor a wrap rule to read it */
} CmdRefusal;

CmdCaptureKind cmdline_capture(Terminal *t, const char *mirror, bool mirror_sure, i32 mirror_caret,
                               CmdInputApp app, char *out, usize cap, usize *typed_from,
                               CmdRefusal *why);

/* Is visible row `row` one of the drawn input's — its rows of text, or the
 * empty ones right under them? A click a program takes can move its caret only
 * there; anywhere else on its screen the caret stays where it was. */
bool cmdline_row_in_input(Terminal *t, i32 row);
/* The same for a mirror that is sure and an app that hides nothing. */
bool cmdline_capture_prompt(Terminal *t, const char *mirror,
                            char *out, usize cap);

/* Keystrokes that erase characters [from, to) from the line: park the cursor
 * on the range's right edge, then backspace over it. Ranges are clamped to the
 * span, so the counts can never over-run the input. */
typedef struct {
    i32 move_right;   /* → presses, before erasing */
    i32 move_left;    /* ← presses, before erasing */
    i32 backspaces;
} CmdErasePlan;

CmdErasePlan cmdline_erase_plan(const CmdSpan *sp, i32 from, i32 to);

/* Build a complete replacement batch, without a submit key. Returns zero
 * if content cannot safely be inserted or the output cannot hold all bytes. */
usize cmdline_recall_bytes(Terminal *t, const CmdSpan *span, const char *command,
                           u8 *out, usize cap);

#endif /* TERMINAL_CMDLINE_H */
