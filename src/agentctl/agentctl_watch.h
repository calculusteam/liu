/*
 * Liu — the folders a team works in, watched for changed files.
 *
 * An agent changes files in more ways than its tools say: a script it runs,
 * a build, a removal, the lead's own edits in its terminal. Agent control
 * watches each folder a job works in and judges every change against what
 * the team's members were doing at that moment (agentctl_server.c). This
 * module only hands the changes over: per file, on one serial queue of its
 * own, about a tenth of a second after they happen.
 *
 * macOS: FSEvents, one stream a folder, per-file events. Linux: inotify on
 * every folder of the tree but the ones agentctl_watch_skip_name() names (a
 * folder made later is watched as it appears, and what it already holds is
 * handed over as made). Elsewhere nothing is watched
 * (agentctl_watch_available() is false) and the team hears of the files its
 * members' tools and command lines name.
 */
#ifndef AGENTCTL_WATCH_H
#define AGENTCTL_WATCH_H

#include "core/types.h"

#include <stdbool.h>
#include <string.h>

/* A path component no change is weighed under: version control, package
 * and build caches, editor state. The watch does not descend into such a
 * folder, and the server drops any change whose path names one. */
static inline bool agentctl_watch_skip_name(const char *name, usize len) {
    static const char *const names[] = {
        ".git", "node_modules", "__pycache__", ".pytest_cache", ".mypy_cache", ".ruff_cache", ".next",
        ".turbo", ".parcel-cache", ".gradle", "DerivedData", ".venv", ".idea", ".DS_Store", "4913",
        "Library", ".cache", ".Trash",
    };
    for (usize i = 0; i < sizeof names / sizeof names[0]; i++)
        if (strlen(names[i]) == len && !strncmp(name, names[i], len)) return true;
    return false;
}

#define AGENTCTL_WATCH_MAX 8                 /* folders watched at once */

enum {
    AGW_REMOVED = 1u << 0,                   /* the path was removed (or renamed away) */
    AGW_DIR     = 1u << 1,                   /* the path is a folder */
    AGW_LOST    = 1u << 2,                   /* the system dropped events under it */
    AGW_CREATED = 1u << 3,                   /* the path was made */
};

/* Called on the watch queue with one batch of changed paths (absolute, as the
 * file system spells them) and their AGW_* flags. */
typedef void (*AgentctlWatchFn)(void *ctx, const char *const *paths, const u32 *flags, usize n);

bool agentctl_watch_available(void);
/* Opens the queue; `fn` gets every batch. False when nothing can be watched. */
bool agentctl_watch_start(AgentctlWatchFn fn, void *ctx);
/* The folders to watch from now on (copied; at most AGENTCTL_WATCH_MAX): a
 * folder no longer listed stops, a new one starts. Returns at once. */
void agentctl_watch_folders(const char *const *dirs, usize n);
/* Runs `fn(ctx)` on the watch queue after `ms`. */
void agentctl_watch_later(u32 ms, void (*fn)(void *ctx), void *ctx);
/* Stops every stream and waits until nothing of this module runs. */
void agentctl_watch_stop(void);

#endif /* AGENTCTL_WATCH_H */
