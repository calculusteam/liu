/*
 * Liu — the shell's half of shell integration.
 *
 * The terminal reads OSC 133 (where a prompt, the typed command and its
 * output begin; how the command ended) and OSC 7 (which folder the shell is
 * in), but a stock zsh emits neither, so the prompt zones, the command record
 * with its exit status, "command finished" and the folder a pane reports
 * would all wait on a shell the user had set up by hand.
 *
 * So Liu gives zsh its half itself, without touching a file of the user's:
 * the pane's zsh is started with ZDOTDIR pointing at a folder of Liu's
 * (ssh_session.c, local_env_build), whose .zshenv puts ZDOTDIR back, reads
 * the user's own .zshenv, and loads a few hooks. fish emits both sequences by
 * itself. bash has no clean way in and is left alone.
 *
 * `shell_integration` (config, default on) turns it off.
 */
#ifndef CORE_SHELL_INTEGRATION_H
#define CORE_SHELL_INTEGRATION_H

#include "core/types.h"

/* Write the shell-side files under $HOME/.liu/shell-integration — only the
 * ones whose content differs from what is there — and put that folder in
 * `dir`. False (and an empty `dir`) when there is no HOME or a write was
 * refused: the caller then starts shells without integration. */
bool shell_integration_install(char *dir, usize cap);

/* Return the generated zsh initialization and integration file
 * contents. */
const char *shell_integration_zshenv_text(void);
const char *shell_integration_zsh_text(void);

#endif /* CORE_SHELL_INTEGRATION_H */
