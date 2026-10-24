/*
 * Liu — the prompt last given to each pane's agent, kept per pane.
 *
 * The problem is not that the prompt is gone; it is that it scrolled. An agent
 * CLI usually runs in the ALT SCREEN, which has no scrollback, so the moment
 * the answer is longer than the window the question that produced it is
 * unreachable. With several tabs open the user is then looking at three panes
 * of output and cannot say which one was asked what. `PaneAgent.activity`
 * answers "what is it doing NOW" off the screen; this answers "what was it
 * asked", and the screen cannot supply it.
 *
 * The source is liu's OWN input mirror, not the agent's disk store and not the
 * cells. That choice is what makes it work for every CLI: `history/live_title.c`
 * needs the tool to publish a session file (few do), a hook needs the tool to
 * ship one, and a screen scrape needs the prompt to still be drawn (it is not).
 * Liu already mirrors every byte it types into a pane —
 * `AppState.translate_input_shadow`, which Translate and autosuggest read — so
 * the prompt is already in hand at the instant Enter is pressed, whatever is
 * on the other end.
 *
 * This module is the buffer arithmetic alone: no AppState, no renderer, no
 * terminal. The wrap and the clip are the parts that go wrong (a prompt clipped
 * mid-codepoint draws a replacement box, one measured in BYTES against a column
 * budget wraps short on accented text and long on emoji), and keeping them pure
 * lets them be checked without a window.
 *
 * The cap is 512 bytes per pane and prompts longer than that are kept as their
 * first 512 with `truncated` set. It is a recall aid, not a transcript: the
 * head of a long prompt identifies it, and the alternative is 8 panes x 32 tabs
 * of unbounded text per window.
 */
#ifndef TERMINAL_PROMPT_CAPTURE_H
#define TERMINAL_PROMPT_CAPTURE_H

#include "core/types.h"

/* Bytes, including the NUL. */
#define PROMPT_RECALL_MAX 512

/* Attachments.
 *
 * A prompt is not always only words: the user drags a screenshot onto the pane
 * and the agent answers about a picture the card would otherwise describe as an
 * empty question. Four is the cap because the card shows them, not because a
 * prompt cannot carry more — a fifth is counted and reported as "+N" rather
 * than silently dropped.
 *
 * The paths live in a heap block allocated by the FIRST attach, not inline:
 * inline they would be 2 KB in every PaneAgent, 512 KiB across 8 panes x 32
 * tabs in every window's AppState, for a feature most panes never use. The cost
 * of that is an OWNER: a PromptRecall is not a value that can be assigned
 * around. It is MOVED (prompt_recall_move) and CLEARED (prompt_recall_clear /
 * prompt_recall_images_clear). The tab/pane code copies PaneAgents by
 * assignment and memsets the vacated slot — a bitwise copy followed by a memset
 * of the source IS a move — but a slot that is dropped without being moved from
 * has to be cleared, or its block leaks. prompt_recall_imgs_live() counts the
 * blocks so a leak shows. */
#define PROMPT_RECALL_MAX_IMGS 4
#define PROMPT_RECALL_IMG_PATH 512

typedef struct {
    char path[PROMPT_RECALL_MAX_IMGS][PROMPT_RECALL_IMG_PATH];
} PromptImgs;

typedef struct {
    char text[PROMPT_RECALL_MAX];
    i32  len;
    f64  time;        /* epoch seconds of the last line folded in — DISPLAY
                       * ONLY, and epoch rather than the monotonic clock the
                       * rest of the tree uses because the panel prints a wall
                       * time ("14:32") rather than an age. An age would have to
                       * tick, and a pane that has gone quiet paints no frames,
                       * so "2m ago" would freeze at two minutes and stay there. */
    bool open;        /* a multi-line prompt is still being composed */
    bool truncated;   /* the prompt was longer than the buffer */

    /* Image files this prompt carried, absolute and already resolved. Two
     * sources feed them and neither can be dropped: a drag-and-drop onto an
     * agent pane (which the agent swallows into an attachment chip, so the path
     * never appears in the input mirror at all) and a path the user typed or
     * pasted as text (which is all Codex and Kimi ever get). */
    PromptImgs *imgs; /* NULL until the first attachment; owned */
    i32  img_count;
    i32  img_extra;   /* attachments past the cap — shown as "+N", never lost */
} PromptRecall;

/* Attachment `k`'s path, or NULL past img_count. */
const char *prompt_recall_img(const PromptRecall *pr, i32 k);

/* Move `src` into `dst`: whatever `dst` held is released first, `src` is left
 * empty. The one way a composed prompt reaches its pane. */
void prompt_recall_move(PromptRecall *dst, PromptRecall *src);

/* Attachment blocks currently allocated, process-wide. Diagnostics: a window
 * that has closed every tab must leave this where it found it. */
i32 prompt_recall_imgs_live(void);

/* One wrapped display line: a byte range within PromptRecall.text. */
typedef struct { i32 off, len; } PromptLine;

/* Release the attachments and reset the text. A PromptRecall must be ZERO
 * before its first clear (calloc, memset, `= {0}`): the clear frees the block
 * it finds in `imgs`, so it cannot double as an initialiser for garbage. */
void prompt_recall_clear(PromptRecall *pr);

/* Is there a committed prompt to show? A prompt still being composed
 * (Shift+Enter, no submit yet) answers false — the badge must not offer a
 * half-typed question as the last thing that was asked.
 *
 * An attachment counts as content on its own: "here is a screenshot" with the
 * words left off is a real prompt, and the card can show it. */
bool prompt_recall_ready(const PromptRecall *pr);

/* Record an image file this prompt carries. `path` must already be absolute
 * and known to exist — this module does no I/O. Duplicates are ignored (the
 * same picture reaches the buffer twice whenever a drop's path is ALSO left in
 * the prompt text). Past the cap the attachment is counted in `img_extra`
 * rather than dropped, so the card can say how many it is not showing.
 * Returns true when a slot was filled. */
bool prompt_recall_attach(PromptRecall *pr, const char *path);

/* Drop the attachments, keeping the text.
 *
 * Separate from prompt_recall_line() on purpose. That function resets the TEXT
 * whenever it is handed a line for a prompt that is not open, and applying the
 * same rule to attachments would erase the pictures staged by a drag-and-drop:
 * a drop stages an image with no text at all, so the Enter that finally commits
 * the prompt arrives at a buffer that is closed and empty and would have wiped
 * exactly what it was meant to carry. The caller therefore says when a previous
 * prompt's attachments go — which is when it starts a new prompt in place. */
void prompt_recall_images_clear(PromptRecall *pr);

/* Fold one submitted line into the recall buffer.
 *
 * `continuation` is true when the Enter that submitted this line inserts a
 * NEWLINE rather than sending the prompt — Shift+Enter and Option+Enter in
 * the agent CLIs. Those lines accumulate; the plain Enter that follows closes
 * the prompt and stamps `time`.
 *
 * `line` is the raw input mirror, so it can carry whatever a paste or a
 * dictation put there: CR/LF become line breaks, tabs become spaces, and every
 * other control byte is dropped (an agent CLI treats a stray \r as SUBMIT).
 */
void prompt_recall_line(PromptRecall *pr, const char *line,
                        bool continuation, f64 now);

/* Commit a prompt that was left open.
 *
 * A multi-line prompt is only closed by the plain Enter that sends it, and the
 * user can walk away from one: Ctrl+C after two lines, or a CLI whose newline
 * key liu cannot recognise as one. The buffer would then stay open and the NEXT
 * prompt would be APPENDED to the abandoned one, which shows as two questions
 * merged into a single badge. The caller closes it on the one signal that means
 * "whatever was being composed is over": the agent started a turn.
 *
 * Returns true when there was something open to close.
 */
bool prompt_recall_close(PromptRecall *pr);

/* One-line summary for the badge: the first non-blank line with runs of
 * whitespace collapsed, clipped to `max_cols` DISPLAY COLUMNS (not bytes) with
 * a trailing ellipsis when anything was dropped. Returns bytes written. */
i32 prompt_recall_summary(const PromptRecall *pr, char *out, usize cap,
                          i32 max_cols);

/* Wrap the prompt to `cols` display columns, breaking at spaces where one is
 * available and mid-word only when a single word does not fit. Writes at most
 * `max_lines` descriptors and returns how many were written; a prompt that
 * needs more lines than that is reported clipped (the caller draws an
 * ellipsis). Explicit newlines always break. */
i32 prompt_recall_wrap(const PromptRecall *pr, i32 cols, PromptLine *out,
                       i32 max_lines);

#endif /* TERMINAL_PROMPT_CAPTURE_H */
