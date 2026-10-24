/*
 * Liu — image lightbox. See lightbox.h for why this is an overlay and not a
 * tab.
 */
#include "ui/lightbox.h"
#include "ui/ui.h"
#include "ui/layout.h"
#include "ui/chrome_palette.h"
#include "core/imgproc.h"
#include "core/image_work.h"
#include "core/checked.h"
#include "core/mem_budget.h"
#include "core/utf8.h"
#include "platform/platform.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "stb_image.h"

/* Declare `draw_text_ex()` here; its definition is in `ui.c`. */
extern void draw_text_ex(Renderer *r, const char *text, f32 x, f32 y,
                         Color fg, i32 max_chars, f32 step);

/* Decode ceiling. Generous on purpose — a Retina screenshot is already ~7.7 MP
 * and this is the feature's most common input — but bounded. Header inspection,
 * decode, premultiply and shrink all run on the lightbox worker; the UI thread
 * only swaps the completed owner and performs the eventual GPU upload. */
#define LB_MAX_DIM   16384
#define LB_MAX_BYTES ((usize)(512u * 1024u * 1024u))

/* The 512 MiB decode limit determines which files may be opened. Decoded pixels
 * remain resident while the overlay is open and are copied to the GPU. Both
 * renderer backends reject images larger than 256 MiB, so larger images are
 * downscaled to fit. At that limit, the shorter side of a 16:9 image remains
 * about 6000 px; zoom is capped at `LB_ZOOM_MAX`. */
#ifndef LB_WORK_MAX_BYTES
#define LB_WORK_MAX_BYTES ((usize)(256u * 1024u * 1024u))
#endif

#define LB_ZOOM_MIN  1.0f
#define LB_ZOOM_MAX  12.0f

/* One decode, owned jointly by the lightbox that asked for it and by the
 * worker performing it. Whoever drops the last reference frees it — so the
 * worker never touches the Lightbox, and teardown never has to wait. */
typedef struct LbDecodeJob {
    pthread_t     thread;
    char          path[IMG_LINK_MAX_PATH];
    u64           generation;
    _Atomic bool  done;
    _Atomic bool  cancel;   /* checked at the decode's stage boundaries */
    _Atomic i32   refs;
    u8           *rgba;
    i32           w, h;
    i64           file_bytes;
    char          err[192];
    /* What this job currently has on the ledger. The source RGBA exists from
     * stbi_load until the result is installed or dropped, and at LB_MAX_BYTES
     * that is half a gigabyte the pressure machinery must see, so it is
     * reported for that whole window and not only as the final bytes at
     * install time. */
    ImgWork       work;
} LbDecodeJob;

/* The permit covers active, abandoned AND completed-but-unadopted jobs.
 * There is no waiting queue: a full quota fails admission without blocking
 * the UI. Keeping a permit until the last reference is gone also bounds
 * results belonging to windows the compositor is not painting. */
#define LB_JOB_LIMIT 2u
#define LB_JOB_BYTES ((usize)1024u * 1024u * 1024u)
static pthread_mutex_t g_lb_quota_mu = PTHREAD_MUTEX_INITIALIZER;
static u32 g_lb_jobs;
static usize g_lb_bytes;

static bool lb_quota_resize(void *ctx, usize before, usize after) {
    (void)ctx;
    pthread_mutex_lock(&g_lb_quota_mu);
    bool ok = true;
    if (after > before) {
        usize add = after - before;
        ok = add <= LB_JOB_BYTES - g_lb_bytes;
        if (ok) g_lb_bytes += add;
    } else {
        g_lb_bytes -= before - after;
    }
    pthread_mutex_unlock(&g_lb_quota_mu);
    return ok;
}

static bool lb_job_admit(void) {
    pthread_mutex_lock(&g_lb_quota_mu);
    bool ok = g_lb_jobs < LB_JOB_LIMIT;
    if (ok) g_lb_jobs++;
    pthread_mutex_unlock(&g_lb_quota_mu);
    return ok;
}

static void lb_job_release(LbDecodeJob *j) {
    if (!j) return;
    if (atomic_fetch_sub_explicit(&j->refs, 1, memory_order_acq_rel) != 1) return;
    if (j->rgba) stbi_image_free(j->rgba);
    (void)img_work_set(&j->work, 0);
    pthread_mutex_lock(&g_lb_quota_mu);
    g_lb_jobs--;
    pthread_mutex_unlock(&g_lb_quota_mu);
    free(j);
}

static bool lb_job_cancelled(void *ctx) {
    LbDecodeJob *j = ctx;
    return atomic_load_explicit(&j->cancel, memory_order_acquire);
}

static void *lb_decode_worker(void *arg) {
    LbDecodeJob *j = arg;
    ImgInput input = {0};
    const ImgLimits limits = { LB_MAX_DIM, 0, LB_MAX_BYTES };
    int iw = 0, ih = 0, comp = 0;
    u8 *rgba = NULL;
    usize expected = 0, actual = 0;
    snprintf(j->err, sizeof j->err, "Could not decode the image (changed, unavailable or over budget)");

    if (!img_input_read(j->path, LB_MAX_BYTES, &j->work,
                        lb_job_cancelled, j, &input)) goto done;
    if (!stbi_info_from_memory(input.data, (int)input.len, &iw, &ih, &comp) ||
        !img_limits_ok(iw, ih, &limits) ||
        !img_rgba_bytes_checked(iw, ih, &expected)) goto done;
    int header_w = iw, header_h = ih;
    if (lb_job_cancelled(j) || !img_input_current(j->path, &input) ||
        !img_work_add(&j->work, expected)) goto done;

    rgba = stbi_load_from_memory(input.data, (int)input.len, &iw, &ih, &comp, 4);
    if (!rgba) goto done;
    /* Validate BEFORE premultiply, shrink or GPU publication. The header
     * and decoder use one immutable allocation, but a malformed format may
     * still disagree with its own header. Such a result fails closed. */
    if (!img_rgba_bytes_checked(iw, ih, &actual) ||
        !img_limits_ok(iw, ih, &limits) || iw != header_w || ih != header_h ||
        lb_job_cancelled(j) || !img_input_current(j->path, &input)) goto done;
    if (actual > expected && !img_work_add(&j->work, actual - expected)) goto done;
    if (actual < expected) img_work_drop(&j->work, expected - actual);
    j->file_bytes = input.id.size;
    img_input_free(&input, &j->work);

    img_premultiply_rgba(rgba, iw, ih);
    if (lb_job_cancelled(j)) goto done;
    if (actual > LB_WORK_MAX_BYTES) {
        i32 target = img_max_dim_for_bytes(iw, ih, LB_WORK_MAX_BYTES);
        if (!img_work_shrink(&j->work, &rgba, &iw, &ih, target)) goto done;
    }
    if (lb_job_cancelled(j) || !img_input_current(j->path, &input)) goto done;
    j->rgba = rgba;
    rgba = NULL;
    j->w = iw;
    j->h = ih;
    j->err[0] = '\0';
done:
    if (rgba) stbi_image_free(rgba);
    img_input_free(&input, &j->work);
    /* A failed/cancelled job retains no pixels, even if its UI never pumps. */
    if (!j->rgba) (void)img_work_set(&j->work, 0);
    atomic_store_explicit(&j->done, true, memory_order_release);
    platform_wake_event_loop();
    lb_job_release(j);
    return NULL;
}

static void lb_release_live_pixels(Lightbox *lb) {
    if (!lb || !lb->rgba) return;
    usize bytes = 0;
    if (img_rgba_bytes_checked(lb->w, lb->h, &bytes))
        mem_budget_adjust(MEM_IMAGE_CPU, -(isize)bytes);
    stbi_image_free(lb->rgba);
    lb->rgba = NULL;
}

bool lightbox_decode_ready(const Lightbox *lb) {
    return lb && lb->job &&
           atomic_load_explicit(&lb->job->done, memory_order_acquire);
}

bool lightbox_decode_running(const Lightbox *lb) { return lb && lb->job; }

/* Give up the decode without waiting for it. If the worker is still inside
 * stb_image it keeps the last reference and frees the job — and its transient
 * accounting — itself. */
static void lb_decode_cancel(Lightbox *lb) {
    if (!lb || !lb->job) return;
    LbDecodeJob *j = lb->job;
    lb->job = NULL;
    atomic_store_explicit(&j->cancel, true, memory_order_release);
    lb_job_release(j);
}

static void lb_decode_reap(AppState *app) {
    Lightbox *lb = app ? &app->lightbox : NULL;
    if (!lightbox_decode_ready(lb)) return;
    LbDecodeJob *j = lb->job;
    lb->job = NULL;

    /* A result the user has moved on from — a second image opened while the
     * first was still decoding — is dropped rather than shown. */
    bool wanted = lb->open && (j->generation == lb->decode_generation);

    if (wanted && j->rgba) {
        lb_release_live_pixels(lb);
        /* Transfer the exact finished charge on the main thread. */
        (void)img_work_set(&j->work, 0);
        lb->rgba = j->rgba;
        j->rgba = NULL;              /* ownership moved into the lightbox */
        lb->w = j->w;
        lb->h = j->h;
        lb->file_bytes = j->file_bytes;
        lb->generation++;
        usize bytes = 0;
        if (img_rgba_bytes_checked(lb->w, lb->h, &bytes))
            mem_budget_adjust(MEM_IMAGE_CPU, (isize)bytes);
        lb->zoom = 1.0f;
        lb->pan_x = lb->pan_y = 0.0f;
        lb->dragging = false;
        lb->err[0] = '\0';
    } else if (wanted) {
        snprintf(lb->err, sizeof(lb->err), "%s",
                 j->err[0] ? j->err : "Could not decode the image");
        if (app) app_show_toast(app, lb->err);
    }
    lb_job_release(j);               /* frees whatever was not installed */
}

void lightbox_free(Lightbox *lb) {
    if (!lb) return;
    /* Unsubscribe rather than join. This waited out an uninterruptible
     * stbi_load of up to LB_MAX_BYTES, on a window close. */
    lb_decode_cancel(lb);
    lb_release_live_pixels(lb);
    lb->w = lb->h = 0;
    lb->open = false;
}

/* CPU-only. This is called for every window, not just paintable windows.
 * Starting/sampling the falling edge here keeps cleanup on wall-clock time
 * while render still owns every GPU operation and the visible fade. */
bool lightbox_pump(AppState *app) {
    if (!app) return false;
    Lightbox *lb = &app->lightbox;
    bool changed = lightbox_decode_ready(lb);
    if (!lb->open && lb->job) {
        lb_decode_cancel(lb);
        changed = true;
    } else {
        lb_decode_reap(app);
    }
    if (!lb->open) {
        bool had_state = lb->rgba || lb->w || lb->h || lb->err[0] ||
                         lb->was_open || lb->close_anim.active;
        bool fading = modal_anim_progress(false, &lb->open_anim, &lb->close_anim,
                                           &lb->was_open, app->dpi_scale,
                                           MODAL_OPEN_DUR_FAST, NULL, NULL, NULL);
        if (!fading) {
            lb_release_live_pixels(lb);
            lb->w = lb->h = 0;
            lb->err[0] = '\0';
            anim_reset(&lb->open_anim);
            anim_reset(&lb->close_anim);
            changed |= had_state;
        }
    }
    return changed;
}

void lightbox_close(AppState *app) {
    if (!app || !app->lightbox.open) return;
    Lightbox *lb = &app->lightbox;
    lb->open = false;
    lb->dragging = false;
    lb->decode_generation++;
    lb_decode_cancel(lb);
    /* Already displayed pixels have a different owner from the cancelled
     * decode. Keep them until the normal fade deadline, even without draw. */
    (void)lightbox_pump(app);
}

static const char *lb_basename(const char *p) {
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

bool lightbox_open(AppState *app, const char *path) {
    if (!app || !path || !*path || strlen(path) >= IMG_LINK_MAX_PATH) return false;
    Lightbox *lb = &app->lightbox;
    lb_decode_reap(app);
    if (lb->job) {
        app_show_toast(app, "An image is still loading");
        return false;
    }

    LbDecodeJob *j = calloc(1, sizeof *j);
    if (!j) { app_show_toast(app, "Could not start image loader"); return false; }
    if (!lb_job_admit()) {
        free(j);
        app_show_toast(app, "Image loaders are busy; try again shortly");
        return false;
    }
    j->work.resize = lb_quota_resize;
    snprintf(j->path, sizeof(j->path), "%s", path);
    j->generation = ++lb->decode_generation;
    atomic_init(&j->done, false);
    atomic_init(&j->cancel, false);
    atomic_init(&j->refs, 2);          /* this lightbox, and the worker */
    if (pthread_create(&j->thread, NULL, lb_decode_worker, j) != 0) {
        atomic_store_explicit(&j->refs, 1, memory_order_relaxed);
        lb_job_release(j);
        app_show_toast(app, "Could not start image loader");
        return false;
    }
    pthread_detach(j->thread);         /* nobody ever joins it */
    lb->job = j;
    snprintf(lb->path, sizeof(lb->path), "%s", path);
    snprintf(lb->name, sizeof(lb->name), "%s", lb_basename(path));
    lb->err[0] = '\0';
    /* No anim_start here: modal_anim_progress() owns both edges, and starting
     * the open curve twice would restart it a frame later. Reopening while the
     * close is still draining is handled there too (it resets the close). */
    lb->open = true;
    return true;
}

/* Human-readable file size, matching the way agents print one. */
static void lb_fmt_size(i64 bytes, char *out, usize n) {
    if (bytes >= 1024 * 1024)
        snprintf(out, n, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    else if (bytes >= 1024)
        snprintf(out, n, "%.1f KB", (double)bytes / 1024.0);
    else
        snprintf(out, n, "%lld B", (long long)bytes);
}

/* The rect the image occupies at the current zoom and pan. `fit` is the
 * scale at zoom 1: contained inside the content box, never upscaled past
 * native pixels — a 32x32 icon blown across a 6K display is not a view of it. */
static void lb_image_rect(const AppState *app, f32 box_x, f32 box_y,
                          f32 box_w, f32 box_h, f32 *ox, f32 *oy,
                          f32 *ow, f32 *oh) {
    const Lightbox *lb = &app->lightbox;
    f32 sx = box_w / (f32)lb->w;
    f32 sy = box_h / (f32)lb->h;
    f32 fit = sx < sy ? sx : sy;
    if (fit > 1.0f) fit = 1.0f;
    f32 s = fit * lb->zoom;
    f32 w = (f32)lb->w * s;
    f32 h = (f32)lb->h * s;
    *ow = w; *oh = h;
    *ox = box_x + (box_w - w) * 0.5f + lb->pan_x;
    *oy = box_y + (box_h - h) * 0.5f + lb->pan_y;
}

/* Keep the image from being dragged entirely off the box. When it is smaller
 * than the box on an axis it stays centred on that axis — a pan that could
 * push a fit-sized image around reads as a bug, not as freedom. */
static void lb_clamp_pan(AppState *app, f32 box_w, f32 box_h) {
    Lightbox *lb = &app->lightbox;
    f32 x, y, w, h;
    lb_image_rect(app, 0, 0, box_w, box_h, &x, &y, &w, &h);
    if (w <= box_w) lb->pan_x = 0.0f;
    else {
        f32 lim = (w - box_w) * 0.5f;
        if (lb->pan_x >  lim) lb->pan_x =  lim;
        if (lb->pan_x < -lim) lb->pan_x = -lim;
    }
    if (h <= box_h) lb->pan_y = 0.0f;
    else {
        f32 lim = (h - box_h) * 0.5f;
        if (lb->pan_y >  lim) lb->pan_y =  lim;
        if (lb->pan_y < -lim) lb->pan_y = -lim;
    }
}

static void lb_content_box(const AppState *app, f32 *x, f32 *y, f32 *w, f32 *h) {
    f32 dpi = app->dpi_scale;
    f32 m   = 48.0f * dpi;
    f32 cap = 52.0f * dpi;           /* caption strip under the image */
    *x = m;
    *y = m;
    *w = (f32)app->fb_width  - m * 2.0f;
    *h = (f32)app->fb_height - m * 2.0f - cap;
    if (*w < 1.0f) *w = 1.0f;
    if (*h < 1.0f) *h = 1.0f;
}

/* The caption: name · dimensions · size, then the key hints, centred under the
 * image, on a plate of their own. The scrim leaves the session legible on
 * purpose, and once the image is zoomed the caption sits over the window's
 * last rows — an agent's status line — where text drawn on the scrim alone ran
 * into the terminal's. One layout for the painter and the driver's state. */
typedef struct {
    char        line[320];
    const char *hint;
    i32         n_line, n_hint;
    f32         line_x, line_y, hint_x, hint_y;
    f32         x, y, w, h;          /* the plate */
} LbCaption;

static void lb_caption(const AppState *app, LbCaption *c) {
    const Lightbox *lb = &app->lightbox;
    f32 dpi = app->dpi_scale;
    f32 ui_cw = 8.0f * dpi, ui_ch = 16.0f * dpi;

    char size_s[32];
    lb_fmt_size(lb->file_bytes, size_s, sizeof(size_s));
    if (lb->rgba)
        snprintf(c->line, sizeof(c->line), "%s  ·  %d×%d  ·  %s",
                 lb->name, lb->w, lb->h, size_s);
    else if (lightbox_decode_running(lb))
        snprintf(c->line, sizeof(c->line), "Loading %s…", lb->name);
    else
        snprintf(c->line, sizeof(c->line), "%s", lb->err[0] ? lb->err : lb->name);
    c->hint = (lb->zoom > 1.001f)
        ? "Esc close  ·  drag to pan  ·  wheel to zoom out  ·  ⌘C copy path"
        : "Esc close  ·  wheel to zoom in  ·  ⌘C copy path";
    c->n_line = (i32)utf8_len((const u8 *)c->line, strlen(c->line));
    c->n_hint = (i32)utf8_len((const u8 *)c->hint, strlen(c->hint));

    /* The caption belongs under the IMAGE, not under the box. Pinned to the
     * box it floats half a screen below a small picture and stops reading as
     * that picture's label. Clamped to the box so a zoomed image — which runs
     * past the box and is clipped — does not push it off-window. */
    f32 bx, by, bw, bh;
    lb_content_box(app, &bx, &by, &bw, &bh);
    f32 img_bottom = by + bh;
    if (lb->rgba && lb->w > 0 && lb->h > 0) {
        f32 ix, iy, iw, ih;
        lb_image_rect(app, bx, by, bw, bh, &ix, &iy, &iw, &ih);
        if (iy + ih < img_bottom) img_bottom = iy + ih;
    }

    f32 fw = (f32)app->fb_width;
    c->line_y = img_bottom + 18.0f * dpi;
    c->hint_y = c->line_y + ui_ch + 6.0f * dpi;
    c->line_x = (fw - (f32)c->n_line * ui_cw) * 0.5f;
    c->hint_x = (fw - (f32)c->n_hint * ui_cw) * 0.5f;

    f32 pad_x = LIGHTBOX_CAPTION_PAD_X_PT * dpi;
    f32 pad_y = LIGHTBOX_CAPTION_PAD_Y_PT * dpi;
    i32 widest = c->n_line > c->n_hint ? c->n_line : c->n_hint;
    c->w = (f32)widest * ui_cw + pad_x * 2.0f;
    c->h = c->hint_y + ui_ch - c->line_y + pad_y * 2.0f;
    c->x = (fw - c->w) * 0.5f;
    c->y = c->line_y - pad_y;
}

void lightbox_caption_rect(const AppState *app, f32 *x, f32 *y, f32 *w, f32 *h) {
    LbCaption c;
    lb_caption(app, &c);
    *x = c.x; *y = c.y; *w = c.w; *h = c.h;
}

void lightbox_render(AppState *app) {
    if (!app) return;
    (void)lightbox_pump(app);
    Lightbox *lb = &app->lightbox;
    Renderer *r = &app->renderer;
    const Theme *t = app->config.theme;
    f32 dpi = app->dpi_scale;

    /* One gate for both directions. MODAL_OPEN_DUR_FAST and MODAL_CLOSE_RATIO
     * are the app-wide tunables, so the lightbox arrives and leaves on the same
     * curves as the palette, the settings panel and every dialog. */
    f32 scale = 1.0f, prog = 1.0f;
    bool render = modal_anim_progress(lb->open, &lb->open_anim, &lb->close_anim,
                                      &lb->was_open, dpi, MODAL_OPEN_DUR_FAST,
                                      &scale, &prog, NULL);
    if (!render) {
        /* The exit has drained — now the pixels can go. Deferred to here rather
         * than to lightbox_close() so the image is still there to fade. */
        lb_release_live_pixels(lb);
        lb->w = lb->h = 0;
        lb->err[0] = '\0';
        /* Park the drained animation. anim_done() short-circuits on `active`
         * and otherwise reads the clock — left armed, a closed lightbox would
         * keep asking platform_time_sec() twice a frame for the rest of the
         * process, here and again in liu_continuous_visual()'s gate. */
        anim_reset(&lb->close_anim);
        return;
    }
    /* Keep the loop at interactive rate while either curve runs. This is only
     * half the job — liu_continuous_visual() in main.c is what WAKES the loop
     * and paces the presents; a visual in one list and not the other animates
     * only while something else happens to produce frames. */
    if (!anim_done(&lb->open_anim) || !anim_done(&lb->close_anim))
        anim_register_until(platform_time_sec() + 0.05);

    /* Scrim. Opaque enough to read the image against, translucent enough that
     * the session underneath is still legible as context. */
    Color scrim = (Color){ 0.0f, 0.0f, 0.0f, 0.82f * prog };
    renderer_draw_rect(r, 0, 0, (f32)app->fb_width, (f32)app->fb_height, scrim);
    renderer_flush_rects(r);

    f32 bx, by, bw, bh;
    lb_content_box(app, &bx, &by, &bw, &bh);

    if (lb->rgba && lb->w > 0 && lb->h > 0) {
        f32 ix, iy, iw, ih;
        lb_image_rect(app, bx, by, bw, bh, &ix, &iy, &iw, &ih);
        /* Scale about the centre, so the overlay reads as arriving and then as
         * leaving rather than cutting at either end. */
        f32 cx = ix + iw * 0.5f, cy = iy + ih * 0.5f;
        f32 dw = iw * scale, dh = ih * scale;
        f32 dx = cx - dw * 0.5f, dy = cy - dh * 0.5f;

        /* Clip to the content box: at zoom the image runs past it, and without
         * this it would paint over the caption. renderer_pop_scissor restores
         * the FULL SCREEN, not an enclosing rect — nothing is pushed around
         * this, so that is what is needed here. */
        renderer_push_scissor(r, bx, by, bw, bh);
        /* Tinted, not plain-cached: the tint's alpha scales the image's own
         * coverage, and without it the picture stayed fully opaque while the
         * scrim faded out from under it — the one part of the overlay that did
         * not leave. White RGB is the identity tint. */
        renderer_draw_image_tinted(r, lb, lb->generation, lb->rgba,
                                   lb->w, lb->h, dx, dy, dw, dh,
                                   (Color){ 1.0f, 1.0f, 1.0f, prog });
        renderer_pop_scissor(r);
    }

    /* The caption, on its plate (lb_caption). The plate is opaque and in the
     * chrome's own surface, so the line reads the same over any session and in
     * a light theme, whose dark text would be lost on the black scrim. */
    LbCaption cap;
    lb_caption(app, &cap);
    ChromePalette cp = chrome_palette_for(t);
    Color plate = cp.surface_neutral; plate.a = prog;
    renderer_draw_rrect_simple(r, cap.x, cap.y, cap.w, cap.h, plate,
                               LIGHTBOX_CAPTION_RADIUS_PT * dpi);

    /* The text in the UI's own cell: the 8 pt step alone left its height to
     * whatever the frame had set before, the terminal's cell when nothing had,
     * so a large terminal font spilled the caption past its plate. */
    f32 ui_cw = 8.0f * dpi, ui_ch = 16.0f * dpi;
    Color fg  = t->fg; fg.a = prog;
    /* The hints are a step back from the line, mixed toward the plate rather
     * than faded: alpha-dimmed text all but vanished on a light plate. */
    Color dim = color_lerp(cp.surface_neutral, t->fg, 0.62f); dim.a = prog;
    renderer_set_ui_scale(r, ui_cw, ui_ch);
    draw_text_ex(r, cap.line, cap.line_x, cap.line_y, fg, cap.n_line, ui_cw);
    draw_text_ex(r, cap.hint, cap.hint_x, cap.hint_y, dim, cap.n_hint, ui_cw);

    /* Own the flushes: the caller cannot know whether this frame drew anything,
     * because the early-out above is what decides it. */
    renderer_flush_rects(r);
    renderer_flush_rrects(r);
    renderer_flush_glyphs(r);
    renderer_reset_ui_scale(r);
}

/* ---- input ------------------------------------------------------------- */

bool lightbox_key(AppState *app, i32 key, u32 mods) {
    if (!app || !app->lightbox.open) return false;
    Lightbox *lb = &app->lightbox;
    if (key == KEY_ESCAPE) { lightbox_close(app); return true; }
    if ((mods & MOD_APP) && (key == 'c' || key == 'C')) {
        (void)app_clipboard_copy(app, lb->path, "Path copied");
        return true;
    }
    if (key == '0') { lb->zoom = 1.0f; lb->pan_x = lb->pan_y = 0.0f; return true; }
    /* Everything else is swallowed while the overlay is up: it is modal, and
     * letting a keystroke through would type into the agent the user cannot
     * see. */
    return true;
}

bool lightbox_mouse_down(AppState *app, f32 x, f32 y) {
    if (!app || !app->lightbox.open) return false;
    Lightbox *lb = &app->lightbox;
    if (!lb->rgba || lb->w <= 0 || lb->h <= 0) {
        lightbox_close(app);
        return true;
    }
    f32 bx, by, bw, bh;
    lb_content_box(app, &bx, &by, &bw, &bh);
    f32 ix, iy, iw, ih;
    lb_image_rect(app, bx, by, bw, bh, &ix, &iy, &iw, &ih);
    bool on_image = (x >= ix && x < ix + iw && y >= iy && y < iy + ih);
    if (on_image && lb->zoom > 1.001f) {
        lb->dragging = true;
        lb->drag_x = x;
        lb->drag_y = y;
        return true;
    }
    /* A click outside the zoomed image dismisses the lightbox. At fit zoom,
     * a click on the image also dismisses it. */
    lightbox_close(app);
    return true;
}

bool lightbox_mouse_up(AppState *app) {
    if (!app || !app->lightbox.open) return false;
    app->lightbox.dragging = false;
    return true;
}

bool lightbox_mouse_move(AppState *app, f32 x, f32 y) {
    if (!app || !app->lightbox.open) return false;
    Lightbox *lb = &app->lightbox;
    if (!lb->rgba || lb->w <= 0 || lb->h <= 0) return true;
    if (!lb->dragging) return true;
    lb->pan_x += x - lb->drag_x;
    lb->pan_y += y - lb->drag_y;
    lb->drag_x = x;
    lb->drag_y = y;
    f32 bx, by, bw, bh;
    lb_content_box(app, &bx, &by, &bw, &bh);
    lb_clamp_pan(app, bw, bh);
    return true;
}

bool lightbox_scroll(AppState *app, f32 dy, bool precise) {
    if (!app || !app->lightbox.open) return false;
    Lightbox *lb = &app->lightbox;
    if (!lb->rgba || lb->w <= 0 || lb->h <= 0) return true;
    /* A trackpad reports pixels and a wheel reports notches; scaling both by
     * the same constant makes one of them useless. */
    f32 step = precise ? dy * 0.006f : dy * 0.14f;
    f32 z = lb->zoom * (1.0f + step);
    if (z < LB_ZOOM_MIN) z = LB_ZOOM_MIN;
    if (z > LB_ZOOM_MAX) z = LB_ZOOM_MAX;
    if (z == lb->zoom) return true;
    /* Keep the centre of the view put: scale the pan with the zoom, so the
     * pixel under the middle stays under the middle. */
    lb->pan_x *= z / lb->zoom;
    lb->pan_y *= z / lb->zoom;
    lb->zoom = z;
    f32 bx, by, bw, bh;
    lb_content_box(app, &bx, &by, &bw, &bh);
    lb_clamp_pan(app, bw, bh);
    return true;
}
