/*
 * Liu — may Settings delete this model file?
 *
 * Settings offers a Delete beside each downloaded model so the user can get
 * the 547 MB Whisper build (or the 440 MB Translate GGUF) back off their disk
 * without hunting for Application Support. The button is only ever drawn for a
 * file Liu itself downloaded, and this is the predicate that decides it.
 *
 * It lives in a header of its own, apart from settings.c's drawing, for the
 * same reason sleep_policy.h and poll_budget.h do: the decision is pure string
 * arithmetic with one destructive consequence, so it can be checked with no
 * window, no renderer and no config.
 *
 * The rule, and why each half is there:
 *
 *   - Both model rows accept a file the user points at with "Choose file",
 *     and that file is THEIRS. It can be a whisper.cpp download they already
 *     had, sitting anywhere on the disk. Removing it would be the app deleting
 *     a user's file rather than reclaiming its own download, so there is no
 *     Delete button for one — not a disabled button, not a warning, simply
 *     absent.
 *   - What Liu downloads always lands in one directory
 *     (`$HOME/Library/Application Support/Liu/models`; LIU_WHISPER_SUBDIR and
 *     LIU_MODEL_SUBDIR are the same string), so ownership is a prefix match.
 *   - The prefix alone is not enough. "…/Liu/models-backup/x.bin" shares it,
 *     and so does anything a "../" walks out through. What follows the
 *     directory therefore has to be a single plain filename.
 */
#ifndef UI_MODEL_DELETE_H
#define UI_MODEL_DELETE_H

#include "core/types.h"

#include <string.h>

/* `path` is the selected model path; `models_dir` is its configured models
 * directory without a trailing slash. Returns false for NULL or empty inputs.
 * The caller supplies the resolved directory so this helper does not infer it
 * from `$HOME`. */
static inline bool model_delete_is_liu_owned(const char *path,
                                             const char *models_dir) {
    if (!path || !path[0] || !models_dir || !models_dir[0]) return false;

    usize dlen = strlen(models_dir);
    /* Tolerate a trailing slash on the directory so a caller that built it
     * with one is not silently told "not ours". */
    while (dlen > 1 && models_dir[dlen - 1] == '/') dlen--;

    if (strncmp(path, models_dir, dlen) != 0) return false;
    if (path[dlen] != '/') return false;          /* "models-backup" is not it */

    const char *rest = path + dlen + 1;
    if (!rest[0]) return false;                   /* the directory itself */
    if (strchr(rest, '/') != NULL) return false;  /* no subdirectory */
    /* No dot entry — which is also what stops "..", the only form of escape
     * that survives the checks above. */
    if (rest[0] == '.') return false;
    return true;
}

#endif /* UI_MODEL_DELETE_H */
