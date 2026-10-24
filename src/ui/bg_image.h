/*
 * Liu — process-wide background-image (wallpaper) decode.
 *
 * Windows are AppStates inside ONE process (liu_window_open), each with its
 * own renderer and its own copy of the config, and a config change is
 * broadcast to all of them. A decode per window would repeat the same
 * stb_image work and RGBA allocation for every window and throw all but one
 * away, because the Metal texture cache below already keys on the same file
 * identity and hands every window the same GPU texture.
 *
 * This is the layer above that cache: subscribers to one file identity share
 * ONE decode. A request that arrives while a decode is running joins it; the
 * last subscriber to leave cancels it and frees the pixels.
 *
 * Identity is path + FileStamp (file identity, size, mtime and ctime), carried
 * unchanged to the renderer. Replacing a file while preserving its size and
 * mtime still produces a different job and GPU cache key.
 *
 * Nothing here blocks. bg_image_release() never joins: a worker whose last
 * subscriber is gone owns the job and frees it on its way out, which is what
 * lets a window close while an 8K decode is in flight.
 *
 * Main-thread API except where noted; the internal state has its own lock.
 */
#ifndef UI_BG_IMAGE_H
#define UI_BG_IMAGE_H

#include "core/types.h"
#include "core/file_stamp.h"

typedef struct BgImageJob BgImageJob;

/* Subscribe to a decode of `path`, joining one already in flight for the same
 * file identity. Returns NULL when the path cannot be stat'd, is not a
 * regular file, or the job table is full — the caller then simply has no
 * wallpaper, the same outcome as a failed decode. */
BgImageJob *bg_image_request(const char *path);

/* True once the decode has finished, successfully or not. NULL-safe. */
bool bg_image_done(const BgImageJob *j);

/* The decoded premultiplied-free RGBA, or NULL when the decode failed or has
 * not finished. Valid until this subscriber's bg_image_release(). */
const u8 *bg_image_pixels(const BgImageJob *j, i32 *w, i32 *h);

/* The immutable file revision this job decoded. Keep the subscription alive
 * while using it. A stale result must be requested again, not cached as the
 * file's newer revision. */
bool bg_image_source_stamp(const BgImageJob *j, FileStamp *out);
bool bg_image_stale(const BgImageJob *j);

/* Drop this subscriber's reference. Never blocks. */
void bg_image_release(BgImageJob *j);

/* Total decodes started this process. The point of the coordinator is that
 * this stays at 1 while N windows show one wallpaper, so it is what the
 * dedup test and LIU_WALLPAPER_PROBE report. */
u32 bg_image_decode_count(void);

/* Live subscribers across all jobs — the other half of the same claim. */
i32 bg_image_subscriber_count(void);

#endif /* UI_BG_IMAGE_H */
