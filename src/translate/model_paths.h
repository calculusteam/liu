/*
 * Liu - canonical on-disk location of the local Translate model.
 *
 * Single source of truth for the GGUF filename and the directory under the
 * user's Application Support that holds it. Shared by
 * translate_config_default() (the presence probe) and the model downloader
 * (the download destination) so the two can never disagree on where the
 * model lives.
 */
#ifndef TRANSLATE_MODEL_PATHS_H
#define TRANSLATE_MODEL_PATHS_H

#include "core/types.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

/* The model file shipped/served for the local Translate backend. Override at
 * configure time only if the model file itself is renamed. */
#ifndef LIU_MODEL_FILENAME
#define LIU_MODEL_FILENAME "Hy-MT1.5-1.8B-1.25bit.gguf"
#endif

/* Expected size (bytes) of the canonical model. CMake passes the real value;
 * this is the fallback for an ad-hoc build. Used as a cheap "is this the
 * right file" probe — an old/incompatible model at the default path has a
 * different size and is treated as not-present so the user re-downloads. */
#ifndef LIU_MODEL_SIZE
#define LIU_MODEL_SIZE 461860704ULL
#endif

/* Directory (relative to $HOME) that holds downloaded models. */
#define LIU_MODEL_SUBDIR "Library/Application Support/Liu/models"

/* Write "$HOME/<subdir>/<filename>" into `out`. Returns `out`, or NULL (and
 * empties `out`) when $HOME is unavailable. */
static inline const char *liu_model_default_path(char *out, usize cap) {
    if (!out || cap == 0) return NULL;
    const char *home = getenv("HOME");
    if (!home || !home[0]) { out[0] = '\0'; return NULL; }
    snprintf(out, cap, "%s/%s/%s", home, LIU_MODEL_SUBDIR, LIU_MODEL_FILENAME);
    return out;
}

/* True iff `path` exists and its size matches the expected model size. This
 * is the "is the model usable" probe: a stale/incompatible file (e.g. an old
 * STQ1_0 packing variant of a different size) fails it, so the UI offers a
 * re-download and the backend refuses rather than decoding garbage. SHA is
 * not re-checked here (too slow per call) — the downloader verifies it. */
static inline bool liu_model_file_ok(const char *path) {
    if (!path || !path[0]) return false;
    struct stat st;
    if (stat(path, &st) != 0) return false;
    return (u64)st.st_size == (u64)LIU_MODEL_SIZE;
}

#endif /* TRANSLATE_MODEL_PATHS_H */
