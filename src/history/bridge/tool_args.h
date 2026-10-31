/*
 * liu-history/bridge - a recorded call as the agent it is written into would
 * have made it.
 *
 * Every agent draws a past tool call from its own tool's name and arguments:
 * Claude Code shows `Update(retry.py)` for an `Edit` with a `file_path`, Kimi
 * `Used StrReplaceFile (retry.py)` for a `path` and an `edit`, OpenCode an edit
 * cell for an `edit` with a `path`. A call handed over under the source's name
 * or with the source's argument names is drawn as an unknown tool with its
 * arguments dumped as JSON, or with its subject missing.
 *
 * So a call is first read for what it MEANS, whatever its source spelled —
 * the file, the command, the pattern, the replacements, the todo list — and
 * then written in the target's vocabulary (BridgeVocab). A call the target has
 * no tool for (an MCP call, an unknown tool) keeps its own name and arguments.
 */
#ifndef HISTORY_BRIDGE_TOOL_ARGS_H
#define HISTORY_BRIDGE_TOOL_ARGS_H

#include "history/bridge/ir.h"

typedef struct cJSON cJSON;

typedef enum {
    BRIDGE_VOCAB_CLAUDE = 0,     /* Claude Code                                  */
    BRIDGE_VOCAB_KIMI,           /* Kimi Code CLI                                */
    BRIDGE_VOCAB_COMMANDCODE,    /* Command Code                                 */
    BRIDGE_VOCAB_OPENCODE_V1,    /* OpenCode 1.x                                 */
    BRIDGE_VOCAB_OPENCODE_V2,    /* OpenCode 2.x                                 */
    BRIDGE_VOCAB_GROK,           /* Grok CLI                                     */
    BRIDGE_VOCAB_PI,             /* Pi                                           */
    BRIDGE_VOCAB_OMP,            /* Oh My Pi: Pi's tools and its own             */
} BridgeVocab;

#define BRIDGE_CALL_EDITS_MAX 32

/* What a recorded call did, read from its arguments in any agent's spelling
 * and from its result. Strings point into `in`, into the IR or into memory
 * this struct owns; all stay valid until bridge_call_facts_free. */
typedef struct {
    BridgeToolKind kind;
    const char *name;           /* the source's own tool name                    */
    cJSON      *in;             /* the arguments as recorded (owned; may be NULL) */
    const char *path;           /* the file or folder it names                   */
    const char *command;        /* a shell command, wrapper (`bash -lc`) removed */
    const char *pattern;        /* a search pattern or a glob                    */
    const char *query;          /* a web search                                  */
    const char *url;            /* a fetched page                                */
    const char *content;        /* a file's whole new content                    */
    const char *description;    /* a short label the call carried                */
    const char *prompt;         /* a subagent's task                             */
    /* Replacements an edit made, as old/new text, from its arguments or from
     * the change its result recorded. */
    struct { const char *old_text, *new_text; } edits[BRIDGE_CALL_EDITS_MAX];
    u32         n_edits;
    /* A todo list: an array of {"text","status"} with status one of
     * "pending", "in_progress", "completed" (owned; NULL when none). */
    cJSON      *todos;
    /* An MCP call's server and tool, when its name says them. */
    char        mcp_server[128];
    char        mcp_tool[128];
    /* From the result: what the call returned, whether it failed, the hunks
     * of a recorded file change, whether it created its file. */
    const char *output;
    bool        failed;
    const char *patch;
    bool        created;
    /* Owned storage behind some of the strings above. */
    char       *owned[BRIDGE_CALL_EDITS_MAX * 2 + 4];
    u32         n_owned;
} BridgeCallFacts;

/* Read `call` (a TOOL_CALL) and its `result` (may be NULL). */
void bridge_call_facts(const BridgeEvent *call, const BridgeEvent *result,
                       BridgeCallFacts *out);
void bridge_call_facts_free(BridgeCallFacts *f);

/* The call in `vocab`: the tool's name into `name` and its arguments as a new
 * object into `*args` (the caller deletes it). A call with no counterpart
 * keeps its own name and arguments. False only when nothing could be built. */
bool bridge_call_in_vocab(BridgeVocab vocab, const BridgeCallFacts *f,
                          char *name, usize name_cap, cJSON **args);

/* The result's TOOL_RESULT for `call` at index `call_index` of `h`, paired by
 * id within the next events, or NULL. */
const BridgeEvent *bridge_result_for(const BridgeHistory *h, usize call_index);

/* Lines a set of unified-diff hunks adds and removes. */
void bridge_patch_counts(const char *hunks, u32 *added, u32 *removed);

/* The hunks as [{oldStart, oldLines, newStart, newLines, lines:[...]}]: the
 * shape Claude Code records an edit in. NULL when none parse. */
cJSON *bridge_patch_structured(const char *hunks);

/* Oh My Pi's todo state, {"op":"init","phases":[{"name","tasks":[{"content",
 * "status"}]}],"storage":"session"}: the details of a `todo` result, which Oh
 * My Pi reads back as the session's list when it resumes. NULL without todos. */
cJSON *bridge_omp_todo_details(const BridgeCallFacts *f);

#endif /* HISTORY_BRIDGE_TOOL_ARGS_H */
