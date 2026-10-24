/*
 * Liu — image-file links in terminal output
 *
 * An AI agent that takes a screenshot prints the path it wrote:
 *
 *     › [image] tmp/win_run.png (584.9KB)
 *
 * and the user wants to look at it. This module answers one question about a
 * visible row — "does this line name a local image file, and where on the row
 * is it?" — so the renderer can mark the row and a click can open it.
 *
 * It exists SEPARATELY from the inline-thumbnail scanner in vt_parser.c. That
 * scanner runs once, on the line feed that completes a line, decodes the file
 * and floats a thumbnail pinned to an absolute scrollback line — a model that
 * requires the primary screen and a program that emits its output line by
 * line. Most agent CLIs, Claude Code among them, run in the ALT SCREEN and
 * repaint with cursor addressing, so that scanner is gated off for them
 * (`MODE_ALT_SCREEN`). This one is stateless: it reads the cells that are on
 * screen right now, so it works the same in the alt screen, in scrollback, in
 * a split pane, and for a row the agent rewrites sixty times a second.
 *
 * What the two DO share is the predicate for "is this an image path", which
 * lives here and is called from both, so the rule exists once.
 */
#ifndef TERMINAL_IMG_LINK_H
#define TERMINAL_IMG_LINK_H

#include "core/types.h"
#include "terminal/terminal.h"

#define IMG_LINK_MAX_PATH 1024

typedef struct {
    /* Cell range of the path token on the row, inclusive. */
    i32  start_col, end_col;
    /* Last non-blank column of the whole row. The badge is anchored past this
     * rather than past `end_col`: an agent writes trailing metadata after the
     * path ("(584.9KB)") and a badge drawn immediately after the token would
     * paint over it. */
    i32  content_end_col;
    /* Resolved absolute path. */
    char path[IMG_LINK_MAX_PATH];
} TermImgLink;

/* Does `s` (length `len`, not NUL-terminated) end in an image extension?
 * Case-insensitive. The ONE predicate — vt_parser.c's inline-preview scanner
 * calls this too. */
bool img_link_has_image_ext(const char *s, usize len);

/* Scan a plain string for every local image file its whitespace-delimited
 * tokens name, resolving relative ones against `cwd`. Writes at most `max`
 * absolute paths into `out`, left to right and deduplicated, and returns how
 * many were written.
 *
 * This is the prompt-recall card's second source of attachments: the agents
 * that draw no chip (Codex, Kimi) leave a dropped path in the prompt as
 * literal text, and a user can always type one. A path containing SPACES is
 * out of reach here by construction — the token boundary is whitespace — which
 * is why the drop handler attaches its exact path directly rather than relying
 * on this pass to find it again.
 *
 * Shares img_link_scan_row's trim / extension / path-like / stat ladder, so a
 * token accepted on screen is accepted in a prompt and vice versa. */
i32 img_link_scan_text(const char *text, const char *cwd,
                       char (*out)[IMG_LINK_MAX_PATH], i32 max);

/* Scan visible row `row` of `t` for a local image file it names.
 *
 * Returns true and fills `out` when the row names a path that (a) ends in an
 * image extension, (b) looks like a path rather than a bare filename, and
 * (c) resolves to a regular file on this machine. (c) is what keeps `ls` output
 * and remote SSH paths from lighting up, and it is answered through a small
 * TTL memo so a row the agent repaints every frame costs no syscalls.
 *
 * `cwd` resolves RELATIVE paths and is what makes this work at all in
 * practice: an agent prints `tmp/win_run.png`, not an absolute path, and
 * `Terminal.cwd` is populated only by OSC 7 — which a stock zsh or bash never
 * emits. The caller passes what it knows (app_terminal_cwd() falls back to the
 * child's real process cwd); NULL means "use Terminal.cwd, or give up on
 * relative paths".
 *
 * Reads only — safe to call from the render path. */
bool img_link_scan_row(Terminal *t, i32 row, const char *cwd, TermImgLink *out);

/* Drop the existence memo when the filesystem may have changed, such as
 * after a file-browser write. */
void img_link_forget_all(void);

/* May the existence check look at this absolute path at all? Only when the
 * mount it falls under is a local filesystem: the check is made with no click,
 * on the render pass, for a path that output named (img_link.c). */
bool img_link_path_is_probeable(const char *path);

#endif /* TERMINAL_IMG_LINK_H */
