/* Main-thread wallpaper subscription and publication; the worker and GPU must
 * agree on the file revision. Kept separate so the actual poll path is tested. */
#ifndef LIU_BG_DECODE_H
#define LIU_BG_DECODE_H
#include "ui/ui.h"
#include "ui/bg_image.h"
#include "platform/platform.h"
#include <stdio.h>
#include <string.h>

static void app_bg_decode_start(AppState *app, const char *path) {
    if (!app || app->bg_job || !path || !path[0]) return;
    snprintf(app->bg_image_requested, sizeof app->bg_image_requested, "%s", path);
    app->bg_job = bg_image_request(path);
    if (!app->bg_job) app->bg_image_requested[0] = '\0';
}

static void app_bg_decode_poll(AppState *app, struct PlatformWindow *window) {
    if (!app || !window) return;
    AppConfig *c = &app->config;
    if (app->bg_job && bg_image_done(app->bg_job)) {
        BgImageJob *job = app->bg_job;
        app->bg_job = NULL;
        bool still_wanted = strcmp(app->bg_image_requested, c->background_image) == 0;
        bool installed = false;
        bool stale = bg_image_stale(job);
        i32 w = 0, h = 0;
        FileStamp source;
        const u8 *px = bg_image_pixels(job, &w, &h);
        if (still_wanted && !stale && px && bg_image_source_stamp(job, &source)) {
            installed = renderer_load_background_rgba_versioned(&app->renderer,
                app->bg_image_requested, px, w, h,
                platform_get_gpu_device(window), &source);
            if (installed)
                snprintf(app->bg_image_loaded, sizeof app->bg_image_loaded,
                         "%s", app->bg_image_requested);
            else stale = bg_image_stale(job); /* changed between check and upload */
        }
        bg_image_release(job);
        app->bg_image_requested[0] = '\0';
        if (still_wanted && !installed) {
            if (stale) {
                /* Retain the current GPU image while a fresh snapshot is decoded.
                 * At most one job belongs to this window; release the old one first. */
                app_bg_decode_start(app, c->background_image);
                if (!app->bg_job) c->background_image[0] = '\0';
            } else c->background_image[0] = '\0';
        }
    }

    if (!c->background_image[0]) {
        if (app->bg_image_loaded[0]) renderer_destroy_background_image(&app->renderer);
        app->bg_image_loaded[0] = '\0';
    } else if (!app->bg_job && strcmp(app->bg_image_loaded, c->background_image) != 0) {
        app_bg_decode_start(app, c->background_image);
    }
}
#endif
