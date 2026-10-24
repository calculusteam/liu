/*
 * Liu — image lightbox
 *
 * An overlay that shows one image over the pane the user is already looking at,
 * dismissed with Esc. It exists instead of routing every click to
 * app_open_image_viewer_tab() because the images this opens are produced BY a
 * conversation the user is in the middle of — an agent's screenshot, a plot it
 * just wrote — and swapping the tab out from under a running session to glance
 * at a picture loses the thing the picture is about. The file browser's viewer
 * (FVIEW_IMAGE) stays what it is: the way to browse images as files.
 *
 * Ownership: AppState holds one Lightbox, so it is per WINDOW. That is
 * deliberate — two windows can each have one open on different files, and the
 * overlay is drawn inside app_render(), which runs per window.
 */
#ifndef UI_LIGHTBOX_H
#define UI_LIGHTBOX_H

#include "core/types.h"
#include "ui/anim.h"
#include "terminal/img_link.h"
#include <pthread.h>
#include <stdatomic.h>

typedef struct AppState AppState;

typedef struct {
    bool  open;
    char  path[IMG_LINK_MAX_PATH];
    char  name[256];          /* basename, for the caption */
    u8   *rgba;               /* premultiplied RGBA, owned here */
    i32   w, h;
    i64   file_bytes;
    /* Bumped on every successful load. renderer_draw_image_cached keys its
     * texture on (address, generation), and the address is this struct — which
     * outlives every image it shows — so without the bump a second image would
     * be drawn with the first one's texture. */
    u64   generation;
    f32   zoom;               /* 1 = fit to window */
    f32   pan_x, pan_y;       /* pixels, from centre */
    bool  dragging;
    f32   drag_x, drag_y;
    /* `open` is the WANT flag; the overlay keeps drawing while close_anim
     * drains, which is what makes the dismissal a fade instead of a cut. Both
     * anims and the edge tracker are driven by anim.h's shared
     * modal_anim_progress(), so the lightbox enters and leaves at the same rate
     * and on the same curves as every other modal in the app. */
    Anim  open_anim;
    Anim  close_anim;
    bool  was_open;
    char  err[192];           /* non-empty => show the message instead */

    /* Decode-only worker; the GPU upload stays on the render thread, when the
     * completed pixels are first drawn.
     *
     * The job is a SEPARATELY OWNED block, not fields in here, refcounted
     * between the lightbox and its worker — the same shape FbImageJob uses.
     * Teardown drops a reference and returns instead of joining an
     * uninterruptible stbi_load of up to LB_MAX_BYTES, and a worker whose
     * lightbox is gone holds the last reference and frees the job itself, so
     * it can never write into freed state. */
    struct LbDecodeJob *job;
    /* The generation a result must match to be installed. Bumped whenever a
     * different image is opened, so a decode the user has moved on from is
     * discarded rather than shown. */
    u64           decode_generation;
} Lightbox;

/* Load `path` and show it. Returns false (and leaves nothing open) when the
 * file cannot be shown; the reason is toasted by the caller. */
bool lightbox_open(AppState *app, const char *path);
void lightbox_close(AppState *app);
/* Free the pixels. Called from app teardown; never waits on a decode. */
void lightbox_free(Lightbox *lb);

/* A decode has finished and is waiting to be installed by the next render.
 * The main loop's completion gate asks this. */
bool lightbox_decode_ready(const Lightbox *lb);
/* A decode is in flight (published or not). */
bool lightbox_decode_running(const Lightbox *lb);

/* Main-thread CPU ownership/animation cleanup, including unpainted windows.
 * Never uploads or touches a GPU context. Returns whether state changed. */
bool lightbox_pump(AppState *app);

/* Does the overlay own input? False the instant it is dismissed — the close
 * animation must not keep swallowing keystrokes on its way out. */
static inline bool lightbox_active(const Lightbox *lb) { return lb && lb->open; }

/* Draw. Call last in app_render so the overlay is above everything — and call
 * it UNCONDITIONALLY, every frame.
 *
 * It early-outs on its own when there is nothing to show, and that early-out is
 * load-bearing: the frame after the close animation drains is where the decoded
 * image is released. Gating the call on "is it open" instead would skip exactly
 * that frame and hold the pixels until the window closed. */
void lightbox_render(AppState *app);

/* The caption's plate, in pixels, exactly as lightbox_render() draws it (the
 * driver's state reports it). */
void lightbox_caption_rect(const AppState *app, f32 *x, f32 *y, f32 *w, f32 *h);

/* Input. Each returns true when the event was consumed and must not reach the
 * terminal underneath. */
bool lightbox_key(AppState *app, i32 key, u32 mods);
bool lightbox_mouse_down(AppState *app, f32 x, f32 y);
bool lightbox_mouse_up(AppState *app);
bool lightbox_mouse_move(AppState *app, f32 x, f32 y);
bool lightbox_scroll(AppState *app, f32 dy, bool precise);

#endif /* UI_LIGHTBOX_H */
