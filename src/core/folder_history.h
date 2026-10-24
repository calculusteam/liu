/*
 * Folder history is derived from project sidecar directories. A command run in
 * a folder creates or updates its sidecar directory, and the command-history
 * file modification time supplies the last-used time. The scanner reads the
 * recorded path from the sidecar because the directory slug is lossy; when the
 * path file is missing, it resolves and records the path once. Ambiguous slug
 * matches are skipped rather than mapped to a potentially incorrect folder.
 * Folders that no longer exist are omitted.
 */
#ifndef LIU_CORE_FOLDER_HISTORY_H
#define LIU_CORE_FOLDER_HISTORY_H

#include "core/types.h"

/* Matches MAX_PALETTE_ITEMS — the picker cannot show more than this, and
 * scanning past it would resolve paths nothing will ever draw. */
#define FOLDER_HISTORY_MAX 256

/* Opaque, caller-owned snapshot. */
typedef struct FolderHistory FolderHistory;

/* The result is owned by an internal single-slot cache; do not free it or
 * retain it past the next folder_history_scan() call. Each call rescans after
 * the Folder History action so the list reflects recent commands. The optional
 * exclude path identifies the current folder and is omitted from the results.
 */
const FolderHistory *folder_history_scan(const char *exclude);

i32         folder_history_count(const FolderHistory *h);        /* 0 if NULL */
const char *folder_history_path(const FolderHistory *h, i32 i);  /* "" if oob */
i64         folder_history_used_at(const FolderHistory *h, i32 i); /* unix s   */

/* Display label for `path`, written to `out`.
 *
 * "$HOME/x" becomes "~/x", and a path too wide for `max_cols` is elided from
 * the FRONT — the tail is the half that identifies a folder, and the palette's
 * row renderer clips from the left, so a label handed over long loses exactly
 * the part worth reading. Whole path components are dropped where they can be,
 * so the result still reads as a path ("…/paket-wordpress-core/02-ssrf").
 *
 * `max_cols` is DISPLAY COLUMNS, not bytes: a Turkish folder name is fewer
 * columns than bytes and an emoji is more, so a byte budget wraps short on one
 * and overruns the row on the other. `home` may be NULL to skip the "~" step.
 */
void folder_history_label(const char *path, const char *home,
                          i32 max_cols, char *out, usize cap);

/* Release the internal cache (process-exit cleanup; optional). */
void folder_history_shutdown(void);

#endif /* LIU_CORE_FOLDER_HISTORY_H */
