/*
 * Liu — reading a delegated agent's structured output.
 *
 * `codex exec --json`, `claude -p --output-format stream-json`, `opencode run
 * --format json` and `gemini -o stream-json` write one JSON object a line. This module turns each line into what the job's tab
 * shows (one short line per command, edit or tool call; the agent's messages
 * whole) and keeps what Liu reports back: the session id the next turn
 * resumes, the agent's final message, the token counts and, for a turn that
 * failed, why.
 *
 * Everything shown comes from a model or from a tool's output, so every piece
 * of text passes agentctl_sanitize() before it is handed on: no escape
 * sequence, no C0 / C1 control (a tab and, where asked, a newline stay), no
 * bidirectional override, no invalid UTF-8. The styling is the caller's: an
 * emitted line carries a style, never an escape sequence.
 *
 * An event this module does not know is shown as one dim line with its type
 * and has no other effect, so a newer CLI that adds events still runs.
 */
#ifndef AGENTCTL_STREAM_H
#define AGENTCTL_STREAM_H

#include "core/types.h"

#include <stdbool.h>

typedef enum {
    AGS_TEXT = 0,   /* the agent's own words */
    AGS_DIM,        /* reasoning, bookkeeping, unknown events */
    AGS_TOOL,       /* a command or a tool call */
    AGS_EDIT,       /* a file the agent changed */
    AGS_OK,
    AGS_ERR,
    AGS_HEAD,       /* a turn's frame lines */
} AgentctlStyle;

/* One logical line (it may hold newlines of its own; no trailing one). */
typedef void (*AgentctlEmit)(void *ctx, AgentctlStyle style, const char *text, usize len);

#define AGENTCTL_STREAM_SESSION_CAP 128
#define AGENTCTL_STREAM_ERROR_CAP   512
#define AGENTCTL_STREAM_USAGE_CAP   128
/* A line longer than this is skipped whole, with a note in its place. */
#define AGENTCTL_STREAM_LINE_MAX    (8u * 1024u * 1024u)
/* The final message kept for the result file. */
#define AGENTCTL_STREAM_FINAL_MAX   (4u * 1024u * 1024u)
/* Claude tool calls waiting for their result at once; one past this is
 * counted when it is made. */
#define AGENTCTL_STREAM_PENDING     8

typedef struct {
    i32   agent;                                   /* AgentctlAgent */
    char  session[AGENTCTL_STREAM_SESSION_CAP];
    char  model[96];                               /* as the agent reported it, "" if not */
    char  usage[AGENTCTL_STREAM_USAGE_CAP];
    /* The turn's spend as numbers: input tokens (cached ones included), the
     * cached part, output tokens, and the cost in millionths of a dollar
     * where the agent reports one (0 otherwise). */
    u64   tok_in, tok_cached, tok_out, cost_micro;
    char  error[AGENTCTL_STREAM_ERROR_CAP];
    char *final_text;                              /* NUL-terminated, sanitized; NULL = none */
    usize final_len;
    bool  turn_completed;
    bool  turn_failed;
    u32   commands, edits, tools, messages;

    char *line;                                    /* the line being assembled */
    usize line_len, line_cap;
    bool  line_skipping;                           /* inside an over-long line */

    u32   notices[8];                              /* hashes of the notices shown this turn */
    u32   nnotices;

    /* Claude's tool calls waiting for their result: a shell command (its line
     * and the folder it started in) or an edit, which counts as a change only
     * once its tool says it worked. */
    struct {
        char  id[64];
        char *cmd;                                 /* a Bash call's line, else NULL */
        char *path;                                /* an edit's file, else NULL */
        char *dir;                                 /* where the command started; NULL = not known */
    } pending[AGENTCTL_STREAM_PENDING];
    u32   npending;
    /* Where Claude's shell is: a `cd` carries over to its next command, back
     * in the job's folder ("") when it leaves it; unknown after a move Liu
     * cannot follow. */
    char  sh_dir[1024];
    bool  sh_dir_known;
    /* Gemini's assistant text arrives in pieces: gathered here and shown
     * whole when something else comes. */
    char *text_acc;
    usize text_len, text_cap;

    /* Set by the caller after init, NULL for none; both get `file_ctx`.
     * Each file the agent changed, by the path it named (sanitized, not cut;
     * relative to the job's folder or absolute). */
    void (*on_file)(void *ctx, const char *path, usize len);
    /* A command the agent runs through its shell starts (`done` false) and
     * ends (`done` true); both carry its line as a person reads it and the
     * folder it started in, relative to the job's ("" for there, NULL when
     * not known). `id` pairs the two. */
    void (*on_command)(void *ctx, const char *id, bool done, const char *cmd, const char *dir);
    /* Claude, asked to put tools to the host: a tool its level refuses.
     * `input` is the call's input as JSON text, owned by the stream for the
     * call only; `what` is a line for a person (sanitized). The host answers
     * on the agent's stdin (agentctl_control_answer). */
    void (*on_ask)(void *ctx, const char *req_id, const char *tool, const char *what, const char *input);
    /* Claude withdrew a request (the turn was interrupted). */
    void (*on_ask_cancel)(void *ctx, const char *req_id);
    /* A control request of any other kind: the host answers it with an
     * error so the agent does not wait. */
    void (*on_control)(void *ctx, const char *req_id);
    void *file_ctx;
} AgentctlStream;

void agentctl_stream_init(AgentctlStream *s, i32 agent);
void agentctl_stream_free(AgentctlStream *s);

/* Bytes from the agent's stdout, in any pieces. */
void agentctl_stream_feed(AgentctlStream *s, const u8 *data, usize len, AgentctlEmit emit, void *ctx);
/* The stream ended: a last line without a newline is read too. */
void agentctl_stream_finish(AgentctlStream *s, AgentctlEmit emit, void *ctx);
/* One whole line, without the line assembly. */
void agentctl_stream_line(AgentctlStream *s, const char *line, usize len, AgentctlEmit emit, void *ctx);

/* The line the host writes on Claude's stdin: the user message that opens a
 * turn (`text` JSON-escaped into it), or the answer to a tool request —
 * `allow` with the call's own input handed back, or a refusal that says why.
 * malloc'd, ending in a newline; NULL when out of memory or `input` is not
 * a JSON object. */
char *agentctl_user_message_json(const char *text, usize len);
char *agentctl_control_answer(const char *req_id, bool allow, const char *input, const char *why);
char *agentctl_control_error(const char *req_id, const char *why);

/* Copies `in` into `out` (always NUL-terminated) without anything a terminal
 * would act on; returns the length written. With `keep_newlines` false a
 * newline becomes a space. Text that does not fit is cut on a character
 * boundary. */
usize agentctl_sanitize(const char *in, usize n, char *out, usize cap, bool keep_newlines);

/* Shell command lines, read the way the shell reads them (quotes, `;` `&&`
 * `|`, subshells, here-documents) and with each `cd` followed. `dir` is the
 * folder the line starts in, as the caller spells it ("" = the caller's own,
 * NULL = not known: then only absolute paths come out). Each path handed to
 * `fn` is joined to the folder its command ran in. */
typedef void (*AgentctlPathFn)(void *ctx, const char *path);
/* What the line's words say it writes, makes or removes: output
 * redirections (not a duplication, nothing under /dev), and the file
 * arguments of tee, touch, rm and mv. Returns how many. */
i32  agentctl_shell_writes(const char *cmd, const char *dir, AgentctlPathFn fn, void *ctx);
/* Every word of the line, quoted or not, that may name a file — the
 * candidates a caller checks on disk after the command ran. At most
 * AGC_NAMES_MAX. Returns how many. */
#define AGC_NAMES_MAX 128
i32  agentctl_command_names(const char *cmd, const char *dir, AgentctlPathFn fn, void *ctx);
/* Every word of `text` (a screen, a log) that looks like a file's name or
 * path — it has a dot or a slash in it, and is not a number or a URL's —
 * at most AGC_TEXT_NAMES_MAX. Returns how many. */
#define AGC_TEXT_NAMES_MAX 4096
i32  agentctl_text_names(const char *text, AgentctlPathFn fn, void *ctx);
/* True when every command of the line only reads or prints (cat, ls, grep,
 * git status, sed without -i…) and nothing is redirected into a file. */
bool agentctl_command_reads_only(const char *cmd);

#endif /* AGENTCTL_STREAM_H */
