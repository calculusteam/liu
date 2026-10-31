/*
 * liu-history/bridge - Universal Agent History IR.
 *
 * The one representation every source parser produces and every target
 * adapter consumes. It exists because pairwise converters are O(N^2): with
 * fourteen agents that is 182 integrations, and each one would have to learn
 * both formats. A source parser here learns exactly one format (its own) and
 * a target adapter learns exactly one (its own).
 *
 * Relationship to ChatEvent (history/event.h): ChatEvent is the *viewer's*
 * model — a flat role + text stream, which is all a transcript pane needs.
 * It cannot round-trip a conversation into another agent because it drops
 * the two things a resumable history is made of: the tool_call <-> tool_result
 * pairing (ChatEvent stores a call id in `tool_name` for results and the tool
 * NAME in the same field for calls, so the link is unrecoverable) and the
 * separation of a tool call's input from its output. BridgeEvent keeps both.
 * ChatEvent is untouched and all eight existing parsers still feed it.
 *
 * Memory model, mirroring EntryVec in scanner.c: the event ARRAY is malloc'd
 * and grows geometrically (a bump arena cannot realloc); every STRING is
 * arena-owned and dies with the caller's arena. So a BridgeHistory is only
 * valid while its arena is, and bridge_history_free releases just the array.
 */
#ifndef HISTORY_BRIDGE_IR_H
#define HISTORY_BRIDGE_IR_H

#include "core/memory.h"
#include "core/types.h"
#include "history/event.h"

/* ---------------------------------------------------------------- events -- */

typedef enum {
    BRIDGE_EVENT_SYSTEM = 0,   /* system-visible metadata worth replaying    */
    BRIDGE_EVENT_USER,
    BRIDGE_EVENT_ASSISTANT,

    BRIDGE_EVENT_TOOL_CALL,
    BRIDGE_EVENT_TOOL_RESULT,

    BRIDGE_EVENT_COMPACTION,   /* the tool's own summary of dropped history  */
    BRIDGE_EVENT_CONTEXT,      /* environment blob (cwd/os/date) the tool    */
                               /* injected; replayable but never user text   */
    BRIDGE_EVENT_UNKNOWN,
} BridgeEventType;

/* Semantic tool identity. See tool_map.h — the point is that a target adapter
 * asks "what did this call DO", never "what was it called in the other CLI". */
typedef enum {
    BRIDGE_TOOL_UNKNOWN = 0,

    BRIDGE_TOOL_FILE_READ,
    BRIDGE_TOOL_FILE_WRITE,
    BRIDGE_TOOL_FILE_PATCH,

    BRIDGE_TOOL_SHELL,

    BRIDGE_TOOL_TEXT_SEARCH,
    BRIDGE_TOOL_FILE_SEARCH,

    BRIDGE_TOOL_WEB_SEARCH,
    BRIDGE_TOOL_WEB_FETCH,

    BRIDGE_TOOL_TASK,
    BRIDGE_TOOL_SUBAGENT,

    BRIDGE_TOOL_MCP,

    BRIDGE_TOOL_KIND_COUNT_,
} BridgeToolKind;

typedef struct {
    BridgeEventType type;

    i64 timestamp_ms;          /* 0 when the source stores none              */

    /* Message text for SYSTEM/USER/ASSISTANT/COMPACTION/CONTEXT. NULL for
     * tool events unless the source attached prose to them. */
    const char *text;

    /* Tool events. `tool_name` is the ORIGINAL name as the source agent wrote
     * it — kept so a same-tool round trip is lossless and so a target that
     * happens to share the name can pass it through. `tool_kind` is the
     * normalized identity every other target keys on. */
    const char    *tool_name;
    BridgeToolKind tool_kind;

    /* Links a TOOL_CALL to its TOOL_RESULT. Every source that stores one is
     * carried through verbatim; when a source stores none, ir.c synthesizes
     * a stable "liucall_<n>" so the pairing still survives into the target.
     * Never NULL on a tool event that reached bridge_history_finish(). */
    const char *tool_call_id;

    /* Compact JSON for a call's arguments; the tool's textual output for a
     * result. Both may be NULL (a call with no arguments, a result that was
     * never recorded). Arguments over BRIDGE_TOOL_INPUT_MAX are stored as
     *   {"liu_truncated":true, "file_path":…, "command":…, "liu_input":"…"}
     * — still an object, with the call's path and command (when it has them,
     * under the keys the source used) and the escaped head of the original. */
    const char *tool_input;
    const char *tool_output;

    /* Whether tool_output was clipped by the size policy. A target that
     * re-serializes it must keep the marker rather than pretend it is whole. */
    bool tool_output_truncated;

    /* What a file-editing call changed, on its TOOL_RESULT, when the source
     * recorded it: unified-diff hunks ("@@ -a,b +c,d @@" and their lines,
     * numbered in the file as it was), or tool_file_created for a call that
     * made the file. NULL and false when the source keeps no such record; a
     * diff over the size policy is not kept at all rather than kept cut. */
    const char *tool_patch;
    bool        tool_file_created;
    /* The source marked this TOOL_RESULT as the call failing (Anthropic's
     * `is_error`). False when it did not, or keeps no such mark. */
    bool        tool_failed;

    /* Verbatim source record, when the source parser could cheaply keep it
     * and a same-tool target could use it. NULL otherwise — never required. */
    const char *raw_payload;
} BridgeEvent;

/* ------------------------------------------------------------- fidelity --- */

typedef enum {
    BRIDGE_FIDELITY_FULL = 0,  /* nothing was dropped or clipped             */
    BRIDGE_FIDELITY_HIGH,      /* only disposable tool output was clipped    */
    BRIDGE_FIDELITY_PARTIAL,   /* whole tool records or metadata were lost   */
    BRIDGE_FIDELITY_HANDOFF,   /* no native history; a summary was written   */
} BridgeFidelity;

/* Counters filled while parsing, reported to the user after an import. Every
 * lossy step in the pipeline must bump one of these — a migration that
 * silently drops half a conversation and reports success is the single worst
 * failure this subsystem can have. */
typedef struct {
    u32  messages;                 /* user + assistant events kept           */
    u32  messages_truncated;       /* must stay 0: see BRIDGE_MESSAGE_MAX    */
    u32  tool_calls;
    u32  tool_results;
    u32  tool_outputs_truncated;   /* outputs clipped, and oversized inputs  */
    u32  events_dropped;           /* hit the event cap or the byte budget,  */
                                   /* or held what no target can take: an    */
                                   /* image the user attached                */
    bool metadata_complete;        /* cwd + title + model all recovered      */
    bool history_complete;         /* nothing dropped at all                 */
} BridgeFidelityStats;

BridgeFidelity bridge_fidelity_of(const BridgeFidelityStats *s);
const char    *bridge_fidelity_name(BridgeFidelity f);

/* -------------------------------------------------------------- history --- */

typedef struct {
    ChatTool    source_tool;
    const char *source_session_id;
    const char *source_path;       /* absolute path of the source transcript */

    const char *cwd;
    const char *title;
    const char *source_model;

    const char *git_root;
    const char *git_branch;
    const char *git_head;

    BridgeEvent *events;           /* malloc'd, geometric growth             */
    usize        event_count;
    usize        event_cap;

    BridgeFidelityStats fidelity;

    /* Internals — do not read from adapters. */
    Arena *arena;                  /* string owner; caller-owned lifetime    */
    usize  bytes_used;             /* against BRIDGE_TOTAL_MAX               */
    u32    synth_call_seq;
    bool   overflowed;
} BridgeHistory;

/* Size policy. Deliberately asymmetric: a user's own words and the model's
 * conclusions are the entire point of the migration and are never clipped
 * within any sane bound, while a 4 MB `cargo build` log replayed into the
 * next agent is pure cost. */
#define BRIDGE_MAX_EVENTS         20000u
#define BRIDGE_MESSAGE_MAX        ((usize)MB(1))
#define BRIDGE_TOOL_INPUT_MAX     ((usize)KB(8))
/* Of an input over that cap, the path it names and the command it runs are
 * kept whole up to this length beside the clipped body: a briefing line each,
 * and the two things the next agent most needs to know about the call. */
#define BRIDGE_TOOL_SUBJECT_MAX   ((usize)512)
#define BRIDGE_TOOL_OUTPUT_MAX    ((usize)KB(16))
#define BRIDGE_TOTAL_MAX          ((usize)MB(24))

#define BRIDGE_TRUNC_MARK "\n[Tool output truncated by Liu during session migration]"

/* Initialize `h` against `arena`. Strings pushed later are arena-owned, so
 * the arena must outlive every read of the history. */
void bridge_history_init(BridgeHistory *h, Arena *arena, ChatTool source_tool);

/* Release the event array. The arena is the caller's and is NOT touched. */
void bridge_history_free(BridgeHistory *h);

/* Metadata setters — each arena-dups. Passing NULL/"" leaves the field alone,
 * so a parser can call them from several places as facts turn up. */
void bridge_history_set_session_id(BridgeHistory *h, const char *id);
void bridge_history_set_path      (BridgeHistory *h, const char *path);
void bridge_history_set_cwd       (BridgeHistory *h, const char *cwd);
void bridge_history_set_title     (BridgeHistory *h, const char *title);
void bridge_history_set_model     (BridgeHistory *h, const char *model);
void bridge_history_set_git       (BridgeHistory *h, const char *root,
                                   const char *branch, const char *head);

/* Append one event. `e`'s string fields are copied into the arena under the
 * size policy above; the caller keeps ownership of whatever it passed in.
 * Returns false when the event was dropped (cap reached, budget exhausted,
 * or an empty event with nothing to carry) — the drop is counted, never
 * silent. Tool events without a tool_call_id get a synthesized one. */
bool bridge_history_push(BridgeHistory *h, const BridgeEvent *e);

/* Convenience wrappers used by nearly every source parser. */
bool bridge_push_message(BridgeHistory *h, BridgeEventType type,
                         i64 ts_ms, const char *text);
bool bridge_push_tool_call(BridgeHistory *h, i64 ts_ms, const char *name,
                           const char *call_id, const char *input_json);
bool bridge_push_tool_result(BridgeHistory *h, i64 ts_ms, const char *name,
                             const char *call_id, const char *output);

/* Attach a recorded file change to the TOOL_RESULT at `index` (see
 * BridgeEvent.tool_patch). False when `index` is not a tool result or the
 * patch is over BRIDGE_TOOL_OUTPUT_MAX, which leaves the event as it was. */
bool bridge_history_set_file_change(BridgeHistory *h, usize index,
                                    const char *patch, bool created);

/* Close the history: resolves orphan tool records, fills in the derived
 * fidelity flags. Call once, after the last push, before handing `h` to a
 * target. Idempotent. */
void bridge_history_finish(BridgeHistory *h);

/* True when the history carries nothing a target could resume from — no user
 * turn and no assistant turn. Every adapter refuses these rather than
 * creating an empty session the user then has to clean up. */
bool bridge_history_is_empty(const BridgeHistory *h);

/* Index of the last event of `type`, or -1. Used by adapters that must know
 * whether the conversation ends on a user turn (some targets require the
 * next turn to be the user's). */
isize bridge_history_last_of(const BridgeHistory *h, BridgeEventType type);

const char *bridge_event_type_name(BridgeEventType t);

#endif /* HISTORY_BRIDGE_IR_H */
