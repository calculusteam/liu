/*
 * Liu — the shell's half of shell integration (see shell_integration.h).
 *
 * The scripts are Liu's own text, kept here as strings and written out at
 * launch, so a dev build and a packaged app carry the same files and nothing
 * has to be found next to the binary.
 */
#include "core/shell_integration.h"
#include "core/atomic_file.h"
#include "core/mkdir_p.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char k_zshenv[] =
    "# Liu shell integration: the way in.\n"
    "#\n"
    "# Liu started this zsh with ZDOTDIR pointing at this folder. ZDOTDIR is given\n"
    "# back first, the user's own .zshenv is read as zsh would have read it, and\n"
    "# only then - in an interactive shell - the hooks are loaded. zsh goes on to\n"
    "# read .zprofile, .zshrc and .zlogin from the restored ZDOTDIR, exactly as it\n"
    "# does without Liu. Nothing here is typed into the shell and no file of the\n"
    "# user's is touched.\n"
    "#\n"
    "# First of all the key for this pane's marks leaves the environment: it is\n"
    "# kept in a variable of this shell, so no program started here inherits it.\n"
    "if [[ -n \"${LIU_MARK_KEY+x}\" ]]; then\n"
    "  typeset -g _liu_mark_key=\"$LIU_MARK_KEY\"\n"
    "  unset LIU_MARK_KEY\n"
    "fi\n"
    "# A command this pane runs before its first prompt (an agent started again\n"
    "# on the session it had) leaves the environment the same way.\n"
    "if [[ -n \"${LIU_RESUME_COMMAND+x}\" ]]; then\n"
    "  typeset -g _liu_resume=\"$LIU_RESUME_COMMAND\"\n"
    "  unset LIU_RESUME_COMMAND\n"
    "fi\n"
    "if [[ -n \"${LIU_ORIG_ZDOTDIR+x}\" ]]; then\n"
    "  export ZDOTDIR=\"$LIU_ORIG_ZDOTDIR\"\n"
    "  unset LIU_ORIG_ZDOTDIR\n"
    "else\n"
    "  unset ZDOTDIR\n"
    "fi\n"
    "{\n"
    "  if [[ -r \"${ZDOTDIR:-$HOME}/.zshenv\" ]]; then\n"
    "    builtin source \"${ZDOTDIR:-$HOME}/.zshenv\"\n"
    "  fi\n"
    "} always {\n"
    "  if [[ -o interactive && -n \"$LIU_SHELL_INTEGRATION_DIR\" &&\n"
    "        -r \"$LIU_SHELL_INTEGRATION_DIR/zsh/liu-integration.zsh\" ]]; then\n"
    "    builtin source \"$LIU_SHELL_INTEGRATION_DIR/zsh/liu-integration.zsh\"\n"
    "  fi\n"
    "}\n";

static const char k_zsh_integration[] =
    "# Liu shell integration for zsh.\n"
    "#\n"
    "# Tells the terminal where the prompt, the typed command and its output begin,\n"
    "# how the command ended, and which folder the shell is in (OSC 133, OSC 7).\n"
    "# It prints nothing a person can see and changes no option of the shell.\n"
    "#\n"
    "# The marks that decide what goes into Liu's command history carry this\n"
    "# pane's key (_liu_mark_key, set by .zshenv): text that merely prints the\n"
    "# same escape codes - a file, a log, a remote host - does not have it.\n"
    "# Switch it off with \"shell_integration\": false in ~/.config/Liu/config.json.\n"
    "\n"
    "[[ -o interactive ]] || return 0\n"
    "(( ${+_liu_integration} )) && return 0\n"
    "typeset -gi _liu_integration=1 _liu_running=0\n"
    "\n"
    "# Each hook switches tracing off for itself and what it calls: under\n"
    "# `set -x` they printed some 190 lines per prompt. The one line that says so\n"
    "# is all a trace shows of them.\n"
    "\n"
    "# $1 percent-encoded into REPLY: every byte outside the unreserved set, so a\n"
    "# space, a line break or a non-ASCII letter arrives whole.\n"
    "_liu_urlencode() {\n"
    "  emulate -L zsh\n"
    "  setopt no_multibyte no_xtrace\n"
    "  local src=$1 ch\n"
    "  local -i i\n"
    "  REPLY=\n"
    "  if [[ $src != *[^a-zA-Z0-9/._~-]* ]]; then\n"
    "    REPLY=$src\n"
    "    return 0\n"
    "  fi\n"
    "  for (( i = 1; i <= ${#src}; i++ )); do\n"
    "    ch=$src[i]\n"
    "    case $ch in\n"
    "      ([a-zA-Z0-9/._~-]) REPLY+=$ch ;;\n"
    "      (*) REPLY+=%${(l:2::0:)$(( [##16] #ch ))} ;;\n"
    "    esac\n"
    "  done\n"
    "}\n"
    "\n"
    "# The folder as a file:// URL.\n"
    "_liu_report_cwd() {\n"
    "  setopt localoptions noxtrace\n"
    "  emulate -L zsh\n"
    "  local REPLY\n"
    "  _liu_urlencode \"$PWD\"\n"
    "  builtin printf '\\e]7;file://%s%s\\a' \"$HOST\" \"$REPLY\"\n"
    "}\n"
    "\n"
    "# Before each prompt: how the last command ended (when there was one), where\n"
    "# the shell is, and \"a prompt of this shell starts here\".\n"
    "_liu_precmd() {\n"
    "  local -i ret=$?\n"
    "  setopt localoptions noxtrace\n"
    "  emulate -L zsh\n"
    "  if (( _liu_running )); then\n"
    "    builtin printf '\\e]133;D;%d;liu=%s\\a' $ret \"${_liu_mark_key-}\"\n"
    "    _liu_running=0\n"
    "  fi\n"
    "  _liu_report_cwd\n"
    "  builtin printf '\\e]133;A;liu=%s\\a' \"${_liu_mark_key-}\"\n"
    "}\n"
    "\n"
    "# Enter was pressed on a command: its output starts here, and this is the\n"
    "# command ($1 is the line as it was typed, every line of it). Reading it\n"
    "# back off the screen took the right prompt with it and lost all but the\n"
    "# last line of a multi-line command. A line the shell's own history is\n"
    "# told to leave out (a leading space under HIST_IGNORE_SPACE) is sent as an\n"
    "# empty one, which means \"not for the history\".\n"
    "_liu_preexec() {\n"
    "  setopt localoptions noxtrace\n"
    "  local -i _liu_skip=0\n"
    "  [[ -o histignorespace && $1 == ' '* ]] && _liu_skip=1\n"
    "  emulate -L zsh\n"
    "  local REPLY=\n"
    "  if (( _liu_skip )); then\n"
    "    builtin printf '\\e]133;C;liu=%s;cmd=\\a' \"${_liu_mark_key-}\"\n"
    "  elif [[ -n $1 ]] && (( ${#1} <= 2048 )); then\n"
    "    _liu_urlencode \"$1\"\n"
    "    builtin printf '\\e]133;C;liu=%s;cmd=%s\\a' \"${_liu_mark_key-}\" \"$REPLY\"\n"
    "  else\n"
    "    builtin printf '\\e]133;C;liu=%s\\a' \"${_liu_mark_key-}\"\n"
    "  fi\n"
    "  _liu_running=1\n"
    "}\n"
    "\n"
    "# The line editor has the cursor: the prompt is over, typing starts here.\n"
    "_liu_line_init() {\n"
    "  setopt localoptions noxtrace\n"
    "  emulate -L zsh\n"
    "  builtin printf '\\e]133;B;liu=%s\\a' \"${_liu_mark_key-}\"\n"
    "}\n"
    "\n"
    "# The hooks go in at the first prompt, after every startup file has had its\n"
    "# say, so they run last and a theme that rebuilds the prompt does not leave\n"
    "# them out.\n"
    "_liu_first_prompt() {\n"
    "  local -i ret=$? _liu_nozle=0\n"
    "  setopt localoptions noxtrace\n"
    "  [[ -o shglob || -o restricted ]] && _liu_nozle=1\n"
    "  # The pane's first command, when Liu gave it one: run here, after every\n"
    "  # startup file and with the user's own options, as a foreground job of\n"
    "  # this shell. Its output is marked as a command's, one that is not for\n"
    "  # the history (an empty cmd=).\n"
    "  if [[ -n ${_liu_resume-} ]]; then\n"
    "    local _liu_cmd=$_liu_resume\n"
    "    unset _liu_resume\n"
    "    builtin printf '\\e]133;C;liu=%s;cmd=\\a' \"${_liu_mark_key-}\"\n"
    "    eval \"$_liu_cmd\"\n"
    "    ret=$?\n"
    "    builtin printf '\\e]133;D;%d;liu=%s\\a' $ret \"${_liu_mark_key-}\"\n"
    "  fi\n"
    "  emulate -L zsh\n"
    "  add-zsh-hook -d precmd _liu_first_prompt\n"
    "  add-zsh-hook precmd _liu_precmd\n"
    "  add-zsh-hook preexec _liu_preexec\n"
    "  # zsh's own line-init dispatcher needs a glob that SH_GLOB turns off\n"
    "  # (`emulate sh`, `emulate ksh`): with it on, an error was printed under\n"
    "  # every prompt. A restricted shell may not redirect. The history needs\n"
    "  # only the marks above.\n"
    "  if (( ! _liu_nozle )); then\n"
    "    if (( ${+functions[add-zle-hook-widget]} )) || autoload -Uz add-zle-hook-widget 2>/dev/null; then\n"
    "      add-zle-hook-widget line-init _liu_line_init 2>/dev/null\n"
    "    fi\n"
    "  fi\n"
    "  _liu_report_cwd\n"
    "  builtin printf '\\e]133;A;liu=%s\\a' \"${_liu_mark_key-}\"\n"
    "  return $ret\n"
    "}\n"
    "\n"
    "autoload -Uz add-zsh-hook\n"
    "add-zsh-hook precmd _liu_first_prompt\n";

const char *shell_integration_zshenv_text(void) { return k_zshenv; }
const char *shell_integration_zsh_text(void)    { return k_zsh_integration; }

/* Is `path` already exactly `text`? */
static bool file_is(const char *path, const char *text) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    usize want = strlen(text), got = 0;
    bool same = true;
    char buf[4096];
    for (;;) {
        usize n = fread(buf, 1, sizeof buf, f);
        if (n == 0) break;
        if (got + n > want || memcmp(buf, text + got, n) != 0) { same = false; break; }
        got += n;
    }
    fclose(f);
    return same && got == want;
}

static bool write_if_different(const char *path, const char *text) {
    if (file_is(path, text)) return true;
    AtomicFileWriter w;
    if (!atomic_file_begin_personal(&w, path)) return false;
    usize len = strlen(text);
    if (fwrite(text, 1, len, w.stream) != len) { atomic_file_abort(&w); return false; }
    return atomic_file_commit(&w);
}

/* Is this path what Liu may point a shell at: a directory (or, with
 * `file`, a plain file) that is OURS, reached through no link, and writable
 * by nobody else? A shell reads these files at every start; "identical
 * content" says nothing about who can change it a moment later. */
static bool shell_path_is_ours(const char *path, bool file) {
    struct stat st;
    if (lstat(path, &st) != 0) return false;
    if (file ? !S_ISREG(st.st_mode) : !S_ISDIR(st.st_mode)) return false;
    return st.st_uid == geteuid() && (st.st_mode & (S_IWGRP | S_IWOTH)) == 0;
}

bool shell_integration_install(char *dir, usize cap) {
    if (dir && cap) dir[0] = '\0';
    const char *home = getenv("HOME");
    if (!dir || !home || home[0] != '/') return false;
    char base[900], zsh[960], path[1024];
    if ((usize)snprintf(base, sizeof base, "%s/.liu/shell-integration", home) >= sizeof base) return false;
    if ((usize)snprintf(zsh, sizeof zsh, "%s/zsh", base) >= sizeof zsh) return false;
    /* Owner-only: a shell reads these at every start. */
    if (!liu_mkdir_p(zsh, 0700)) return false;
    /* mkdir -p leaves what was already there as it found it: ~/.liu and the
     * two folders under it are checked, not assumed (another owner, a link,
     * group- or world-writable: no integration, and no ZDOTDIR). */
    char top[900];
    if ((usize)snprintf(top, sizeof top, "%s/.liu", home) >= sizeof top) return false;
    if (!shell_path_is_ours(top, false) || !shell_path_is_ours(base, false) ||
        !shell_path_is_ours(zsh, false)) return false;
    static const struct { const char *name; const char *text; } files[] = {
        { ".zshenv", k_zshenv }, { "liu-integration.zsh", k_zsh_integration },
    };
    for (usize i = 0; i < sizeof files / sizeof files[0]; i++) {
        if ((usize)snprintf(path, sizeof path, "%s/%s", zsh, files[i].name) >= sizeof path) return false;
        /* A file that is there and is not plainly Liu's is replaced,
         * not trusted for having the right bytes today. */
        struct stat st;
        if (lstat(path, &st) == 0 && !shell_path_is_ours(path, true) && unlink(path) != 0) return false;
        if (!write_if_different(path, files[i].text)) return false;
        if (chmod(path, 0600) != 0 || !shell_path_is_ours(path, true)) return false;
    }
    if ((usize)snprintf(dir, cap, "%s", base) >= cap) { dir[0] = '\0'; return false; }
    return true;
}
