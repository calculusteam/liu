/*
 * Liu - on-demand downloader for the local Translate model.
 *
 * The ~440 MB GGUF model is NOT bundled with the app; the user fetches it
 * from Settings on demand. One background worker thread streams the file via
 * curl into a temp ".part" sibling of the destination, the main thread polls
 * progress (bytes, EMA speed, ETA) every frame, and on completion the worker
 * verifies the SHA-256 and atomically renames the file into place.
 *
 * The default URL / SHA-256 / size are baked at configure time
 * (LIU_MODEL_URL, LIU_MODEL_SHA256, LIU_MODEL_SIZE); pass NULL / 0 to
 * model_download_start to use them.
 */
#ifndef TRANSLATE_MODEL_DOWNLOAD_H
#define TRANSLATE_MODEL_DOWNLOAD_H

#include "core/types.h"

typedef enum {
    MODEL_DL_IDLE = 0,
    MODEL_DL_DOWNLOADING,
    MODEL_DL_VERIFYING,
    MODEL_DL_DONE,
    MODEL_DL_ERROR,
    MODEL_DL_CANCELLED,
} ModelDownloadState;

typedef struct {
    ModelDownloadState state;
    u64  bytes_done;
    u64  bytes_total;     /* expected size; 0 if unknown */
    f64  speed_bps;       /* EMA-smoothed bytes/sec */
    f64  eta_sec;         /* seconds remaining, or -1 if unknown */
    char dest_path[1024]; /* final path, populated when state == DONE */
    char error[160];      /* human-readable reason when state == ERROR */
} ModelDownloadStatus;

/* The configure-time defaults. */
const char *model_download_default_url(void);
const char *model_download_default_sha256(void);
u64         model_download_default_size(void);

/* Canonical destination ($HOME/Library/Application Support/Liu/models/<file>).
 * Writes into `out`; returns `out` or NULL when $HOME is unavailable. */
const char *model_download_default_dest(char *out, usize cap);

/* Begin a background download of `url` -> `dest`, verifying
 * `expected_sha256` (64 hex chars, NULL to skip) and `expected_size`
 * (0 = unknown). NULL `url`/`dest`/`sha` and 0 `size` fall back to the
 * configure-time defaults. Non-blocking. Returns false if a download is
 * already in flight or $HOME is unavailable for the default dest. */
bool model_download_start(const char *url, const char *dest,
                          const char *expected_sha256, u64 expected_size);

/* Main-thread progress snapshot; safe to call every frame. */
void model_download_poll(ModelDownloadStatus *out);

/* True while a download or verification is in flight. */
bool model_download_active(void);

/* Ask the worker to abort: kills curl and removes the partial file. */
void model_download_cancel(void);

/* Join the worker (aborting any in-flight transfer first). Idempotent;
 * call once on app shutdown. */
void model_download_shutdown(void);

#endif /* TRANSLATE_MODEL_DOWNLOAD_H */
