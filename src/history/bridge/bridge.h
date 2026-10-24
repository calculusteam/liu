/*
 * liu-history/bridge - cross-agent session continuation.
 *
 * A conversation started in one coding agent, continued inside another as if
 * the second had been there all along:
 *
 *     source store -> source parser -> BridgeHistory -> target adapter
 *                                                    -> target-native session
 *
 * The IR in the middle is mandatory (see ir.h): it is what keeps this at
 * O(N) integrations instead of O(N^2), and it is why neither the UI nor this
 * orchestrator contains one line of per-agent behaviour. Everything an agent
 * needs to know about itself lives in its BridgeTarget.
 *
 * Three rules hold across every adapter, and each one is a rule because the
 * alternative silently destroys somebody's work:
 *
 *   1. The SOURCE session is never touched. Not renamed, not appended to,
 *      not read with anything but O_RDONLY|O_NOFOLLOW.
 *   2. An EXISTING target session is never modified. Every migration mints a
 *      new target session id; a collision is a hard failure, not a merge.
 *   3. Historical tool calls are HISTORY, never instructions. Nothing in this
 *      subsystem executes a recorded shell command, applies a recorded patch,
 *      calls a recorded MCP tool or replays a recorded web action. They are
 *      re-serialized as records of things that already happened.
 *
 * Credentials are the fourth rule and it is enforced by omission: the IR has
 * no field that could carry an auth token, an API key, OAuth state, a
 * permission-bypass flag or a sandbox setting, so no adapter can propagate
 * one even by accident. A migrated session starts under the target agent's
 * own current permission policy.
 */
#ifndef HISTORY_BRIDGE_BRIDGE_H
#define HISTORY_BRIDGE_BRIDGE_H

#include "history/bridge/ir.h"
#include "history/event.h"
#include "history/session.h"

/* How a target takes foreign history, best first. The UI shows this, so the
 * names have to mean something to a user, not just to the adapter. */
typedef enum {
    BRIDGE_IMPORT_UNSUPPORTED = 0,
    /* The agent exposes an API for seeding a thread's model-visible history
     * (codex `thread/inject_items`). Nothing is guessed and nothing is
     * written by hand — the agent itself persists the result. */
    BRIDGE_IMPORT_NATIVE_HISTORY,
    /* The agent ships a documented import command over its own export format
     * (opencode `export` / `import`). We produce that format; the agent
     * validates it and creates the session. */
    BRIDGE_IMPORT_NATIVE_FILE,
    /* No import surface, but the on-disk session format is stable and has
     * been verified against the installed version, so Liu writes it. Always
     * version-gated; an unknown version degrades rather than writing. */
    BRIDGE_IMPORT_INTERNAL_WRITER,
    /* No safe way to seed native history. A structured summary is written and
     * the target is launched pointing at it. This is NOT a resume and the UI
     * must never call it one. */
    BRIDGE_IMPORT_CONTEXT_HANDOFF,
} BridgeImportMode;

const char *bridge_import_mode_name(BridgeImportMode m);
/* Short user-facing capability word for the picker: "Native", "Compatible",
 * "Experimental", "Handoff". */
const char *bridge_import_mode_badge(BridgeImportMode m);

#define BRIDGE_SESSION_ID_CAP 128
#define BRIDGE_PATH_CAP       1024
#define BRIDGE_CMD_CAP        1024
#define BRIDGE_ERR_CAP        512

typedef struct {
    bool             success;
    ChatTool         target_tool;
    BridgeImportMode mode;

    char session_id  [BRIDGE_SESSION_ID_CAP];
    char session_path[BRIDGE_PATH_CAP];

    /* The command that opens the migrated session, ready to run in a pane.
     * Filled by the adapter because only it knows the flag: `codex resume
     * <id>`, `opencode --session <id>`, `claude --resume <uuid>`. Empty when
     * the import failed. */
    char launch_cmd[BRIDGE_CMD_CAP];

    bool history_complete;
    bool tool_history_complete;
    bool metadata_complete;

    BridgeFidelity fidelity;
    u32            warnings;

    char error[BRIDGE_ERR_CAP];
} BridgeImportResult;

/* ------------------------------------------------------------- adapters -- */

typedef struct BridgeTarget BridgeTarget;
struct BridgeTarget {
    ChatTool         tool;
    BridgeImportMode mode;

    /* Is this agent usable on this machine right now? Binary on PATH plus
     * whatever else the adapter needs (a store directory, a daemon). Cheap:
     * called once per picker open, for every target. */
    bool (*available)(void);

    /* Can it take a full history INCLUDING tool records, or only the
     * conversation? Answered at runtime because it often depends on the
     * installed version. */
    bool (*supports_full_history)(void);

    /* Write the migration. Must create a NEW target session, fill `result`,
     * and never touch anything that already existed. */
    bool (*import_history)(const BridgeHistory *history,
                           BridgeImportResult *result);

    /* Prove the session the adapter just claimed to create is really there
     * and really loadable — an adapter that only checks "did fwrite return"
     * will happily hand the user a launch command for a session the target
     * agent then refuses. Called by bridge_migrate; a false answer turns the
     * whole migration into a failure and no session is launched. */
    bool (*validate_import)(const BridgeImportResult *result);
};

/* Look up an adapter. NULL when the tool has none at all. */
const BridgeTarget *bridge_target_for(ChatTool tool);

/* The effective mode for `tool` on THIS machine right now: UNSUPPORTED when
 * not installed or with no adapter, the adapter's own mode when its version
 * gate passes, CONTEXT_HANDOFF when the gate fails but a handoff is possible.
 * This is the single function the UI asks; it must never guess. */
BridgeImportMode bridge_capability(ChatTool tool);
/* bridge_capability without waiting for the version probe — for the agent
 * picker on the main thread. `*pending` is set while a version is still being
 * asked for (the answer is then provisional); ask again when
 * bridge_compat_generation() moves. */
BridgeImportMode bridge_capability_peek(ChatTool tool, bool *pending);

/* ---------------------------------------------------------- orchestrator -- */

/* Parse `meta`'s session into `out` using `arena` for strings. Returns false
 * when the source has no usable parser or the transcript yields nothing.
 * This is the ONLY expensive call in the subsystem — the history LIST stays
 * metadata-only, and this runs when the user picks "Continue with…". */
bool bridge_parse_source(Arena *arena, const ChatSessionMeta *meta,
                         BridgeHistory *out);

/* True when Liu can turn `tool`'s sessions into a BridgeHistory at all. */
bool bridge_source_supported(ChatTool tool);

/* The whole flow: find the source session, parse it, hand it to the target
 * adapter, validate, record lineage. `result` is always filled — on failure
 * `result->error` says why, in a sentence a user can act on.
 *
 * Does not launch anything: the caller runs `result->launch_cmd` in whatever
 * pane it decides, which keeps this file free of UI and of session plumbing. */
bool bridge_migrate(const ChatSessionMeta *source,
                    ChatTool target_tool,
                    BridgeImportResult *result);

/* Same, when the caller has already parsed the source (the tests, and any
 * path that wants to inspect fidelity before committing). */
bool bridge_migrate_history(const BridgeHistory *history,
                            ChatTool target_tool,
                            BridgeImportResult *result);

/* Create `dir` and every missing parent, 0700. Refuses when the FINAL
 * component already exists as a symlink or a non-directory — that component
 * is the session directory an adapter is about to fill, so a symlink there is
 * a redirect. An ancestor that is a symlink is followed but must resolve to a
 * directory; see the comment in writer_common.c for why that asymmetry is the
 * right one. */
bool bridge_mkdir_p(const char *dir);

/* Directory Liu owns for generated sessions and handoff documents:
 * ~/.local/state/liu/bridge (XDG state), created 0700 on demand. Adapters
 * that need scratch space use this, never /tmp — these files contain the
 * user's whole conversation. */
bool bridge_state_dir(char *out, usize cap);

/* Fresh, collision-checked session identifiers.
 *   bridge_new_uuid     — RFC 4122 v4 text, for targets keyed by UUID.
 *   bridge_new_short_id — 26 lowercase base32 chars, for targets that want an
 *                         opaque token (opencode's `ses_…`, kimi, grok).
 * Both draw from /dev/urandom and fall back to a time+pid mix. */
bool bridge_new_uuid(char *out, usize cap);
bool bridge_new_short_id(char *out, usize cap);

/* Milliseconds since the epoch, for adapters stamping generated records. */
i64  bridge_now_ms(void);

/* Run `argv` with no shell, capturing at most `out_cap-1` bytes of its stdout.
 * Adapters use this for version probes and for native import commands. The
 * argv form is the point: nothing this subsystem builds ever reaches /bin/sh,
 * so a session id or a path with a quote in it cannot become a command.
 * Returns the exit status, or -1 when the process could not be run. */
i32 bridge_run_capture(const char *const argv[], char *out, usize out_cap,
                       i32 timeout_sec);

/* Same, but also writes `stdin_data` to the child's stdin and then CLOSES it.
 * Only usable for a child that reads its whole input and exits — see
 * BridgeProc below for the ones that do not. */
i32 bridge_run_capture_stdin(const char *const argv[],
                             const char *stdin_data, usize stdin_len,
                             char *out, usize out_cap, i32 timeout_sec);

/* An interactive line-protocol child keeps stdin open while requests and
 * responses are interleaved. The app-server returns a thread ID that later
 * requests use to address the session. Read protocol lines from stdout; send
 * stderr to /dev/null so banners cannot be mistaken for protocol data. */
typedef struct BridgeProc BridgeProc;

/* Spawn `argv` with pipes on stdin and stdout. NULL on failure. */
BridgeProc *bridge_proc_start(const char *const argv[]);

/* Write `data` to the child's stdin, keeping it open. False on a broken pipe. */
bool bridge_proc_send(BridgeProc *p, const char *data, usize len);

/* Read stdout lines until one contains `needle` (a substring test, so a
 * caller passes something like "\"id\":2"), or the deadline passes. Returns a
 * malloc'd copy of that line, or NULL. Lines that do not match are discarded
 * — the app-server interleaves notifications with responses. */
char *bridge_proc_await_line(BridgeProc *p, const char *needle, i32 timeout_sec);

/* Close stdin, wait briefly, then kill and reap. Always call this. */
void bridge_proc_stop(BridgeProc *p);

/* Is `binary` on PATH? Uses the process PATH agent_ensure_process_path()
 * repaired at startup, so it works from a .app launch. */
bool bridge_binary_available(const char *binary);

#endif /* HISTORY_BRIDGE_BRIDGE_H */
