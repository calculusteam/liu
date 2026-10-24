/*
 * Some adapters write into another application's private session store. Each
 * writer declares its supported version range. A version in range uses the
 * adapter's configured mode; a version outside the range or one that cannot
 * be read uses CONTEXT_HANDOFF. A version condition does not return
 * UNSUPPORTED.
 */
#ifndef HISTORY_BRIDGE_COMPATIBILITY_H
#define HISTORY_BRIDGE_COMPATIBILITY_H

#include "core/types.h"
#include "history/event.h"

typedef struct {
    ChatTool    tool;
    const char *binary;        /* $PATH probe / `--version` target       */
    /* Inclusive bounds, dotted numeric, NULL for "no bound on this side".
     * A bound is only ever widened after the format has actually been
     * re-verified against that version. */
    const char *min_version;
    const char *max_version;
    bool        supports_native_import;  /* has a real import API/command */
    bool        supports_writer;         /* Liu may write its store       */
    /* Argv used to ask the binary its version. Most agents answer
     * `--version`; a few print a banner instead and need something else. */
    const char *version_arg;
} BridgeCompatibility;

/* The compatibility row for `tool`, or NULL when Liu has no writer for it. */
const BridgeCompatibility *bridge_compat_for(ChatTool tool);

/* Read the installed version of `tool` into `out`. Cached per process (a
 * version probe is a process spawn, and the picker asks about every agent on
 * every open). Returns false when the binary is missing or said nothing that
 * looks like a version. WAITS for the probe — one already running is waited
 * for, not repeated — so the main thread asks bridge_compat_version_peek. */
bool bridge_compat_installed_version(ChatTool tool, char *out, usize cap);

/* The installed version WITHOUT waiting for it, for a caller that must not
 * block (the agent picker on the main thread): FOUND or MISSING once a probe
 * has answered; PENDING while one is still to come, in which case a worker
 * thread asks the binary, bumps bridge_compat_generation() and calls the wake
 * hook. */
typedef enum {
    BRIDGE_VERSION_PENDING,
    BRIDGE_VERSION_FOUND,
    BRIDGE_VERSION_MISSING,
} BridgeVersionState;
BridgeVersionState bridge_compat_version_peek(ChatTool tool, char *out, usize cap);
/* Moves whenever a probe finishes or the cache is dropped. */
u32  bridge_compat_generation(void);
/* Called (from the worker's thread) when a probe it ran finishes: the app's
 * loop wake. NULL for none. */
void bridge_compat_set_wake(void (*wake)(void));
/* bridge_compat_writer_ok without waiting: while the version is still to come
 * (`*pending`) the answer is what a version inside the range gives, and the
 * caller asks again when bridge_compat_generation() moves. */
bool bridge_compat_writer_ok_peek(ChatTool tool, bool *pending);

/* Does the installed version fall inside the verified range?
 * `*out_version` (optional) receives what was detected, or "" when unknown.
 * A tool with no compatibility row answers false. */
bool bridge_compat_writer_ok(ChatTool tool, char *out_version, usize cap);

/* Compare dotted numeric versions. Non-numeric tails ("0.149.1-rc.2",
 * "v1.18.23") are tolerated: the leading numeric components decide, and a
 * missing component reads as 0, so "1.2" < "1.2.1". Returns <0, 0, >0. */
i32 bridge_version_cmp(const char *a, const char *b);

/* Extract the first version-looking token ("1.18.23") out of arbitrary CLI
 * output, which is routinely a banner rather than a bare number. False when
 * there is none. */
bool bridge_version_extract(const char *text, char *out, usize cap);

/* Clear the process-wide version cache before rescanning installed
 * agents. */
void bridge_compat_reset_cache(void);

#endif /* HISTORY_BRIDGE_COMPATIBILITY_H */
