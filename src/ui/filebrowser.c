/*
 * Liu - file browser implementation
 * Local filesystem + SFTP browsing, markdown/code viewer.
 */
#include "ui/filebrowser.h"
#include "ui/fb_asset_icons.h"
#include "ui/icon.h"
#include "ui/anim.h"          /* ui_hover_amount — the shared hover engine */
#include "ui/panel_ink.h"      /* its tints, as washes solved for linear blending */
#include "ui/chrome_palette.h"
#include "ui/md_live_lines.h"   /* the live editor's line classes and heights */
#include "ui/line_scroll.h"      /* a wheel event as whole lines */
#include "ui/markdown/md_parse.h"
#include "ui/markdown/md_render.h"
#include "ui/markdown/md_link_target.h"
#include "ui/markdown/md_image.h"
#include "core/imgproc.h"
#include "ui/svg_raster.h"      /* an SVG drawn to pixels, for the image view */
#include "core/mem_budget.h"
#include "ui/markdown/md_graph.h"
#ifdef HAVE_FB_ICON
#include "ui/fb_icon.h"
#include "ui/seti_icons.h"
#else
typedef struct {
    u32             codepoint;
    Color           color;
    FbAssetIconKind asset;
} FbIcon;

static FbIcon fb_icon_for(const char *name, bool is_dir) {
    (void)name;
    (void)is_dir;
    return (FbIcon){0};
}
#endif
#include "ui/layout.h"
#include "core/checked.h"
#include "core/atomic_file.h"
#include "core/config.h"
#include "core/utf8.h"
#include "platform/platform.h"
#include "ssh/ssh_session.h"
#include "ssh/sftp.h"          /* sftp_name_is_safe_component — listing gate */

static void fb_md_embed_roots(FileBrowser *fb);   /* md_embed confinement roots */
#include <stdlib.h>
#include <ctype.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <strings.h>   /* strcasecmp — glibc declares it here, not in string.h */
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>

/* Forward declare editor helper */
static void editor_pre_edit(FileBrowser *fb);
static i32 editor_line_len(FileBrowser *fb, i32 line);
static i32 editor_line_eol(FileBrowser *fb, i32 line);
static char *editor_line_start(FileBrowser *fb, i32 line);
static void editor_clamp_cursor(FileBrowser *fb);
static bool editor_delete_selection(FileBrowser *fb);
static bool ed_sel_valid(const FileBrowser *fb);
static void ed_sel_order(const FileBrowser *fb, i32 *l0, i32 *c0, i32 *l1, i32 *c1);
static void fb_md_reparse(FileBrowser *fb);
static void fb_close_viewer_now(FileBrowser *fb, bool keep_graph);
static void fb_syn_invalidate(FileBrowser *fb);
static bool editor_apply(FileBrowser *fb, usize off, usize oldlen,
                         const char *newbytes, usize newlen);
static void undo_pending_free(struct FbUndoPending *p);
/* The local stat worker (defined with it further down). */
static u64  fb_stat_new_id(void);
static void fb_stat_listing_changed(FileBrowser *fb);
static void fb_stat_forget(u64 fb_id);
static void fb_tree_restore_clear(FileBrowser *fb);
static void fb_tree_restore_capture(FileBrowser *fb);
static void fb_tree_restore_apply(FileBrowser *fb);
static void fb_reveal_apply(FileBrowser *fb);
static void fb_tree_start_pending(FileBrowser *fb);

/* The line-anchor cache names an offset INSIDE view_content; it is keyed on
 * the buffer pointer, and a pointer is not a document identity across a
 * free()+malloc() pair — the allocator often hands the same block back for
 * the next file of a similar size, so a cache left standing over a reopen
 * would re-label a byte of the NEW file as the old file's line N and the next
 * edit would land there. Every site that replaces the buffer drops it; a
 * realloc that keeps the content does not need to. */
static inline void editor_line_cache_drop(FileBrowser *fb) {
    if (!fb) return;
    fb->editor_line_cache_valid = false;
    fb->editor_line_cache_buffer = NULL;
    fb->editor_line_cache_offset = 0;
    fb->editor_line_cache_line = 0;
    fb_syn_invalidate(fb);
}
static void fb_editor_doc_stats(const FileBrowser *fb, u32 *words, u32 *chars, u32 *read_min);
static void fb_graph_job_drop(FileBrowser *fb);     /* let go of a scan; never waits for it */
static bool fb_image_decode_start(FileBrowser *fb, const char *path);
static void fb_image_decode_reap(FileBrowser *fb);
static void fb_image_decode_cancel(FileBrowser *fb);
static void fb_sftp_list_cancel(FileBrowser *fb);
static void fb_local_list_cancel(FileBrowser *fb);
static void fb_md_parse_cancel(FileBrowser *fb);
static void fb_md_parse_reap(FileBrowser *fb);
static bool fb_md_parse_ready(const FileBrowser *fb);
static void fb_md_build(FileBrowser *fb);
static bool fb_text_open_start(FileBrowser *fb, const char *path, const char *fallback, FileViewMode mode);
static void fb_open_set_edit(FileBrowser *fb, bool edit);
static bool fb_open_file_edit(FileBrowser *fb, i32 index, bool edit);
static bool fb_md_open_active(const FileBrowser *fb);
static void fb_image_state_reset(FileBrowser *fb);
static void md_scratch_release(FileBrowser *fb);
#include <dirent.h>
#include <sys/stat.h>

#include <libssh2.h>
#include <libssh2_sftp.h>

#include "stb_image.h"

/* =========================================================================
 * Unsaved edits, and what the browser has to say
 *
 * `modified` is read by the amber pip and by Save, and everything that FREES
 * the text — opening another file, closing the viewer, quitting — asks
 * fb_discard_ok(), which refuses the first time and lets the repeat through
 * (ui/unsaved_guard.h). A refusal, a failed save and a reload leave one line
 * in FileBrowser.notice for whoever holds the browser to show.
 * ========================================================================= */

/* The open document's name, for a notice. */
static const char *fb_view_name(const FileBrowser *fb) {
    const char *base = strrchr(fb->view_path, '/');
    return (base && base[1]) ? base + 1 : fb->view_path;
}

static void fb_notice_stamp(FileBrowser *fb, bool error) {
    fb->notice_error = error;
    fb->notice_at = platform_time_sec();
    fb->notice_seq++;
    if (fb->notice_seq == fb->notice_taken) fb->notice_seq++;   /* wrapped onto "taken" */
}

static void fb_notice(FileBrowser *fb, bool error, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
static void fb_notice(FileBrowser *fb, bool error, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(fb->notice, sizeof fb->notice, fmt, ap);
    va_end(ap);
    fb_notice_stamp(fb, error);
}

void fb_notice_set(FileBrowser *fb, bool error, const char *text) {
    if (fb && text) fb_notice(fb, error, "%s", text);
}

bool fb_notice_take(FileBrowser *fb, char *out, usize cap, bool *is_error) {
    if (!fb || fb->notice_seq == fb->notice_taken || !fb->notice[0]) return false;
    fb->notice_taken = fb->notice_seq;
    if (out && cap) snprintf(out, cap, "%s", fb->notice);
    if (is_error) *is_error = fb->notice_error;
    return true;
}

static bool fb_view_is_document(const FileBrowser *fb) {
    return fb->view_mode == FVIEW_TEXT || fb->view_mode == FVIEW_CODE ||
           fb->view_mode == FVIEW_MARKDOWN;
}

bool fb_editor_dirty(const FileBrowser *fb) {
    return fb && fb->modified && fb->view_content && fb_view_is_document(fb);
}

/* May `action` take the open text away? Yes when nothing in it is unsaved, and
 * for the confirming repeat. Otherwise no — and the notice says what to do. */
static bool fb_discard_ok(FileBrowser *fb, UnsavedAction action) {
    if (unsaved_guard_allow(&fb->guard, action, fb_editor_dirty(fb), platform_time_sec()))
        return true;
    unsaved_guard_message(fb->notice, sizeof fb->notice, action, fb_view_name(fb));
    fb_notice_stamp(fb, true);
    return false;
}

bool fb_unsaved_confirm(FileBrowser *fb, UnsavedAction action) {
    return fb ? fb_discard_ok(fb, action) : true;
}

void fb_editor_discard(FileBrowser *fb) {
    if (!fb) return;
    fb->modified = false;
    unsaved_guard_disarm(&fb->guard);
}

/* Longest-edge cap applied to the full-screen image viewer's decoded buffer.
 *
 * The viewer always aspect-fits into the content area (there is no zoom path),
 * so the only thing that matters is covering the largest panel a real display
 * can present in *backing* pixels — ~3000 px on a 5K/6K panel, comfortably less
 * on anything portable.
 *
 * On memory: a 16:9 frame lands at 2560x1440 = 14.7 MB and fits the renderer's
 * per-texture pool-admission limit (16 MiB on both backends), so landscape
 * photos — the common case — stay in the ordinary LRU. Squarer aspect ratios
 * exceed it (2560x2560 = 26 MB) and go to the renderer's dedicated oversize
 * slot instead. That slot is a real cache, so those are uploaded once too; they
 * simply don't share the pool. Bounding the long edge rather than the area is
 * therefore a sharpness choice, not a performance one. */
/* Ceiling on a document held in the editor. fb_open_file refuses a file over 2
 * MB; this bounds what EDITING may grow one to, and with it the two
 * full-document undo mirrors that shadow it. */
#define FB_EDIT_MAX_BYTES ((usize)64 * 1024 * 1024)

/* Ceiling on everything undo retains: the two mirrors plus every frame's
 * spliced bytes. The frames are deltas, so ordinary editing is a few bytes
 * each — but one frame holds a whole pasted blob, and 32 of those are 32
 * copies of it. Over the budget the oldest frame is folded into the base,
 * which is the same reclamation the 32-frame limit already performs, so undo
 * gets shallower rather than the editor getting heavier. */
#define FB_UNDO_MAX_BYTES ((usize)192 * 1024 * 1024)

#define FB_VIEW_IMAGE_MAX_DIM 2560
/* Animated frames are uploaded every time their generation advances. Keep one
 * frame below the renderer's 16 MiB oversize path so the Metal three-frame
 * texture ring and the GL in-place update path can both be used; 1920² RGBA is
 * 14.1 MiB and still exceeds the pixels available in the viewer. */
#define FB_VIEW_IMAGE_ANIM_MAX_DIM 1920

/* Ceiling on the decoded RGBA the viewer KEEPS for an animated image, and the
 * smallest long edge an animation may be shrunk to in order to stay under it.
 *
 * A single still tops out at FB_VIEW_IMAGE_MAX_DIM^2 * 4 = 26 MB, but an
 * animation multiplies that by its frame count and the buffer stays resident
 * for as long as the file is open — a backgrounded tab included. Frames are
 * dropped only after shrinking has been pushed to FB_VIEW_IMAGE_ANIM_MIN_DIM,
 * because a slightly softer animation is a much smaller surprise than a loop
 * that cuts off early. */
#define FB_VIEW_IMAGE_ANIM_MAX_BYTES ((i64)64 * 1024 * 1024)
#define FB_VIEW_IMAGE_ANIM_MIN_DIM   1024

/* Bound frame-decoding work independently of file size; a compact animation can
 * declare many tiny frames. */
#define FB_VIEW_IMAGE_ANIM_MAX_FRAMES 4096

/* Live orphaned decoders, process-wide: workers whose FileBrowser is gone and
 * which are still inside stb_image. Each pins at most one decode's buffers
 * (the limits below allow 128 MiB), so this is what bounds the memory a burst
 * of closes can hold. Past the cap fb_image_decode_cancel joins instead —
 * a bounded stall traded for a bounded footprint. */
#define FB_IMAGE_MAX_ORPHANS 3
static _Atomic i32 g_fb_image_orphans;

/* One decode, owned jointly by the FileBrowser that asked for it and by the
 * worker performing it. Whoever drops the last reference frees it, so the
 * worker never touches the FileBrowser and a teardown never has to wait. */
typedef struct FbImageJob {
    pthread_t     thread;
    char          path[FB_MAX_PATH];
    _Atomic bool  done;        /* results are readable once this is set */
    _Atomic bool  cancel;      /* checked at the decode's stage boundaries */
    _Atomic bool  orphan;      /* holds one g_fb_image_orphans slot */
    _Atomic i32   refs;
    bool          ok;
    bool          svg;         /* the file is an SVG: drawn by svg_rasterize_file, not decoded */
    bool          svg_dark;    /* ...and its ink is mostly dark */
    SvgStatus     svg_status;  /* why it was not drawn, when !ok */
    u8           *rgba;
    i32          *delays;
    i32           w, h, frames;
    /* What this job has on the ledger right now. The viewer admits a 128 MiB
     * still or a 64 MiB animation, and the decode is where memory pressure
     * needs to see it, so the bytes are reported before the RESULT is
     * installed. Released by fb_image_job_release on every path. */
    usize         transient;
} FbImageJob;

static void fb_image_job_release(FbImageJob *j) {
    if (!j) return;
    if (atomic_fetch_sub_explicit(&j->refs, 1, memory_order_acq_rel) != 1)
        return;
    if (j->transient)
        mem_budget_transient_adjust(MEM_IMAGE_CPU, -(isize)j->transient);
    if (atomic_load_explicit(&j->orphan, memory_order_acquire))
        atomic_fetch_sub_explicit(&g_fb_image_orphans, 1, memory_order_release);
    if (j->rgba) stbi_image_free(j->rgba);
    free(j->delays);
    free(j);
}

static void *fb_image_decode_worker(void *arg) {
    FbImageJob *job = arg;
    const char *path = job->path;
    const char *ext = strrchr(path, '.');
    bool gif_file = ext && (strcasecmp(ext, ".gif") == 0);
    const bool svg_file = fb_path_is_svg(path);
    u8 *px = NULL;
    i32 w = 0, h = 0, frames = 1;
    i32 *delays = NULL;

    const ImgLimits limits = {
        .max_dim = 0,
        .max_pixels = (usize)256 * 1024 * 1024,
        .max_rgba_bytes = (usize)128 * 1024 * 1024,
    };
    if (atomic_load_explicit(&job->cancel, memory_order_acquire)) goto done;
    if (svg_file) {
        /* Drawn at the viewer's largest size, off this thread like every other
         * decode; svg_rasterize_file bounds the file, the raster and the time
         * (an estimate from the geometry, since nanosvg cannot be stopped). The
         * raster is on the ledger from here, before it exists, like a still's. */
        job->svg = true;
        usize pre = 0;
        if (img_rgba_bytes_checked(FB_VIEW_IMAGE_MAX_DIM, FB_VIEW_IMAGE_MAX_DIM, &pre)) {
            job->transient += pre;
            mem_budget_transient_adjust(MEM_IMAGE_CPU, (isize)pre);
        }
        const SvgLimits svg_limits = {
            .max_file_bytes = SVG_DEFAULT_MAX_FILE_BYTES,
            .long_edge = FB_VIEW_IMAGE_MAX_DIM,
            .max_rgba_bytes = limits.max_rgba_bytes,
        };
        SvgImage svg;
        job->svg_status = svg_rasterize_file(path, &svg_limits, &svg);
        if (job->svg_status != SVG_OK) goto done;
        if (atomic_load_explicit(&job->cancel, memory_order_acquire)) {
            free(svg.rgba); goto done;
        }
        px = svg.rgba;
        w = svg.w; h = svg.h;
        job->svg_dark = svg.dark_ink;
    } else if (gif_file) {
        FILE *f = fopen(path, "rb");
        if (!f || fseek(f, 0, SEEK_END) != 0) { if (f) fclose(f); goto done; }
        long sz = ftell(f);
        if (sz <= 0 || sz > 32 * 1024 * 1024 || fseek(f, 0, SEEK_SET) != 0) {
            fclose(f); goto done;
        }
        u8 *src = malloc((usize)sz);
        if (!src) { fclose(f); goto done; }
        /* The compressed file plus, once img_gif_load_bounded runs, up to
         * FB_VIEW_IMAGE_ANIM_MAX_BYTES of frames. Accounted before either
         * exists; the release reconciles. */
        job->transient += (usize)sz;
        mem_budget_transient_adjust(MEM_IMAGE_CPU, (isize)sz);
        bool read_ok = fread(src, 1, (usize)sz, f) == (usize)sz;
        fclose(f);
        if (!read_ok) { free(src); goto done; }
        const ImgGifPolicy policy = {
            .max_dim = 0,
            .max_frame_bytes = limits.max_rgba_bytes,
            .max_frames = FB_VIEW_IMAGE_ANIM_MAX_FRAMES,
            .max_total_bytes = (usize)FB_VIEW_IMAGE_ANIM_MAX_BYTES,
            .shrink_dim = FB_VIEW_IMAGE_ANIM_MAX_DIM,
            .shrink_min_dim = FB_VIEW_IMAGE_ANIM_MIN_DIM,
        };
        ImgGif gif;
        if (atomic_load_explicit(&job->cancel, memory_order_acquire)) {
            free(src); goto done;
        }
        ImgGifResult gr = img_gif_load_bounded(src, (usize)sz, &policy, &gif);
        free(src);
        job->transient -= (usize)sz;
        mem_budget_transient_adjust(MEM_IMAGE_CPU, -(isize)sz);
        if (gr != IMG_GIF_OK) goto done;
        px = gif.px; delays = gif.delays;
        w = gif.w; h = gif.h; frames = gif.frames;
        {   /* The decoded frame set, which is what the viewer will hold. */
            usize one = 0, all = 0;
            if (img_rgba_bytes_checked(w, h, &one) &&
                checked_mul_usize(one, (usize)(frames > 0 ? frames : 1), &all)) {
                job->transient += all;
                mem_budget_transient_adjust(MEM_IMAGE_CPU, (isize)all);
            }
        }
    } else {
        i32 hw = 0, hh = 0;
        if (img_preflight_path(path, &limits, &hw, &hh) != IMG_PREFLIGHT_OK)
            goto done;
        int ch = 0;
        {   /* From the header, before stbi_load allocates it. */
            usize pre = 0;
            if (img_rgba_bytes_checked(hw, hh, &pre)) {
                job->transient += pre;
                mem_budget_transient_adjust(MEM_IMAGE_CPU, (isize)pre);
            }
        }
        px = stbi_load(path, &w, &h, &ch, 4);
        if (!px || !img_limits_ok(w, h, &limits) ||
            atomic_load_explicit(&job->cancel, memory_order_acquire)) {
            if (px) stbi_image_free(px);
            px = NULL; goto done;
        }
        /* stb_image cannot be interrupted, so cancellation is checked at the
         * stage boundaries around it. The premultiply and the shrink over a
         * 128 MiB buffer are not cheap, and skipping them is most of the
         * orphan's remaining life. */
        img_premultiply_rgba(px, w, h);
        if (!img_shrink_to_max_dim(&px, &w, &h, FB_VIEW_IMAGE_MAX_DIM)) {
            stbi_image_free(px); px = NULL; goto done;
        }
    }

    job->rgba = px;
    job->delays = delays;
    job->w = w;
    job->h = h;
    job->frames = frames;
    job->ok = true;
done:
    atomic_store_explicit(&job->done, true, memory_order_release);
    platform_wake_event_loop();
    fb_image_job_release(job);
    return NULL;
}

static bool fb_image_decode_start(FileBrowser *fb, const char *path) {
    if (!fb || !path || !path[0] || fb->image_job) return false;
    /* The image takes the viewer, and the text in it is freed below. */
    if (!fb_discard_ok(fb, UNSAVED_OPEN)) return false;

    /* Commit to the new viewer before launching, but keep all heavy disk and
     * pixel work off this thread. */
    fb_image_state_reset(fb);
    if (fb->md_images) { md_image_cache_destroy(fb->md_images); fb->md_images = NULL; }
    if (fb->md_embeds) { md_embed_cache_destroy(fb->md_embeds); fb->md_embeds = NULL; }
    if (fb->md_arena.base) arena_destroy(&fb->md_arena);
    fb_md_parse_cancel(fb); free(fb->md_src); fb->md_src = NULL;
    fb->md_doc = NULL;
    md_scratch_release(fb);

    editor_line_cache_drop(fb);
    free(fb->view_content);
    fb->view_content = malloc(1);
    if (!fb->view_content) { fb->view_cap = 0; return false; }
    fb->view_content[0] = '\0'; fb->view_size = 0; fb->view_cap = 1;
    fb->view_edit_gen++;
    fb->view_disk = (FileIdentity){0};
    snprintf(fb->view_path, sizeof fb->view_path, "%s", path);
    fb->view_mode = FVIEW_IMAGE;
    fb->editor_mode = false; fb->modified = false;
    fb->view_scroll = 0; fb->view_scroll_px = 0.0f;
    fb->view_content_px = 0.0f; fb->view_scroll_x = 0;
    fb->hscroll_accum_px = 0.0f; fb->vscroll_accum_px = 0.0f; fb->view_line_count = 0;

    FbImageJob *j = calloc(1, sizeof *j);
    if (!j) return false;
    snprintf(j->path, sizeof j->path, "%s", path);
    atomic_init(&j->done, false);
    atomic_init(&j->cancel, false);
    atomic_init(&j->orphan, false);
    atomic_init(&j->refs, 2);          /* this FileBrowser, and the worker */
    if (pthread_create(&j->thread, NULL, fb_image_decode_worker, j) != 0) {
        free(j);
        return false;
    }
    fb->image_job = j;
    return true;
}

bool fb_image_decode_ready(const FileBrowser *fb) {
    return fb && fb->image_job &&
           atomic_load_explicit(&fb->image_job->done, memory_order_acquire);
}

/* Give up the decode without waiting for it. If the worker is still inside
 * stb_image it keeps the last reference and frees the job itself; the cancel
 * flag makes that tail as short as an uninterruptible decode allows. */
static void fb_image_decode_cancel(FileBrowser *fb) {
    if (!fb || !fb->image_job) return;
    FbImageJob *j = fb->image_job;
    fb->image_job = NULL;
    atomic_store_explicit(&j->cancel, true, memory_order_release);

    if (atomic_load_explicit(&j->done, memory_order_acquire)) {
        /* Already finished — the thread is on its way out or gone. Joining is
         * free and keeps the orphan slots for decodes that really are stuck. */
        pthread_join(j->thread, NULL);
        fb_image_job_release(j);
        return;
    }
    if (atomic_load_explicit(&g_fb_image_orphans, memory_order_acquire)
            < FB_IMAGE_MAX_ORPHANS) {
        atomic_fetch_add_explicit(&g_fb_image_orphans, 1, memory_order_acq_rel);
        atomic_store_explicit(&j->orphan, true, memory_order_release);
        pthread_detach(j->thread);     /* nobody will ever join it */
        fb_image_job_release(j);
        return;
    }
    pthread_join(j->thread, NULL);
    fb_image_job_release(j);
}

static void fb_image_decode_reap(FileBrowser *fb) {
    if (!fb || !fb->image_job) return;
    FbImageJob *j = fb->image_job;
    if (!atomic_load_explicit(&j->done, memory_order_acquire)) return;
    /* `done` is a release store the worker makes after its last write, so the
     * acquire above is the whole handshake. The thread is reaped by the
     * release below. */
    fb->image_job = NULL;
    pthread_join(j->thread, NULL);
    bool wanted = fb->view_mode == FVIEW_IMAGE &&
                  strcmp(fb->view_path, j->path) == 0;
    if (wanted && j->ok && j->rgba) {
        fb->view_image_rgba = j->rgba;
        fb->view_image_frame_delays = j->delays;
        fb->view_image_w = j->w;
        fb->view_image_h = j->h;
        fb->view_image_frame_count = j->frames;
        fb->view_image_frame_index = 0;
        fb->view_image_frame_anchor = platform_time_sec();
        fb->view_image_next_frame_at = 0.0;
        fb->view_image_is_animated = j->frames > 1;
        fb->view_image_svg = j->svg;
        fb->view_image_ink_dark = j->svg_dark;
        fb->view_image_gen++;
        fb->view_image_zoom = 1.0f;
        fb->view_image_pan_x = fb->view_image_pan_y = 0.0f;
        usize one = 0, all = 0;
        if (img_rgba_bytes_checked(fb->view_image_w, fb->view_image_h, &one) &&
            checked_mul_usize(one, (usize)fb->view_image_frame_count, &all))
            mem_budget_adjust(MEM_IMAGE_CPU, (isize)all);
        j->rgba = NULL;          /* ownership moved into the viewer */
        j->delays = NULL;
        fb_editor_reset_undo(fb);
    }
    /* An SVG that could not be drawn is still a text file: say why, and show
     * its source instead of a blank viewer. */
    char svg_path[FB_MAX_PATH];
    svg_path[0] = '\0';
    SvgStatus svg_why = SVG_OK;
    if (wanted && !j->ok && j->svg) {
        snprintf(svg_path, sizeof svg_path, "%s", j->path);
        svg_why = j->svg_status;
    }
    fb_image_job_release(j);     /* frees whatever was not installed */
    if (svg_path[0]) {
        const char *name = strrchr(svg_path, '/');
        fb_notice(fb, true, "Cannot draw %s (%s): showing its source",
                  name ? name + 1 : svg_path, svg_status_text(svg_why));
        if (!fb_text_open_start(fb, svg_path, svg_path, FVIEW_CODE))
            fb->view_mode = FVIEW_NONE;
    }
}

/* =========================================================================
 * Init / Destroy
 * ========================================================================= */

void fb_init(FileBrowser *fb) {
    if (!fb) return;
    memset(fb, 0, sizeof(*fb));
    fb->hover_entry = -1;
    fb->selected = -1;
    fb->selection_anchor = -1;
    fb->md_seek_block = MD_SEEK_NONE;   /* 0 is a valid block index */
    fb->width = 240;
    fb->entries = NULL;
    fb->entry_cap = 0;
    fb->graph_drag_slider = -1;   /* 0 is a valid slider index; -1 = none */
    /* Identity for the stat worker: never reused, so a result for a browser
     * that was destroyed and a new one at the same address cannot match. */
    fb->stat_id = fb_stat_new_id();
    fb->stat_gen = 1;
}

void fb_entry_set_name(FileEntry *e, const char *src) {
    if (!e) return;
    free(e->name);
    if (!src) { e->name = NULL; return; }
    /* Filesystems on external volumes (HFS+, ExFAT, some SMB shares) return
     * filenames in NFD-decomposed form: "ş" comes back as "s" + U+0327
     * combining cedilla. Liu's renderer pushes one glyph per codepoint at a
     * fixed cell width, so the combining mark lands in its own cell where
     * it visually disappears (the mark is offset off the cell's printable
     * box). Normalizing to NFC up front turns the pair back into a single
     * precomposed codepoint that the font has a real glyph for. */
    char buf[1024];
    if (platform_utf8_normalize_nfc(src, buf, sizeof buf)) {
        src = buf;
    }
    usize n = strlen(src);
    e->name = malloc(n + 1);
    if (e->name) memcpy(e->name, src, n + 1);
}

void fb_entries_reset(FileBrowser *fb) {
    if (!fb) return;
    /* Whatever the worker still owes this listing is owed to nobody now. */
    fb_stat_listing_changed(fb);
    if (!fb->entries) { fb->entry_count = 0; return; }
    for (i32 i = 0; i < fb->entry_count; i++) {
        free(fb->entries[i].name);
        fb->entries[i].name = NULL;
        free(fb->entries[i].path);
        fb->entries[i].path = NULL;
    }
    fb->entry_count = 0;
}

/* =========================================================================
 * Multi-selection bitmap
 *
 * FB_MAX_ENTRIES is small enough (1024) that a flat bitmap fits in 128 bytes.
 * `selection_count` is the popcount cache; keeping it accurate in set/toggle
 * lets callers short-circuit on empty selections without scanning 128 bytes.
 * ========================================================================= */

void fb_sel_clear(FileBrowser *fb) {
    if (!fb) return;
    memset(fb->selection_set, 0, sizeof(fb->selection_set));
    fb->selection_count = 0;
    fb->selection_anchor = -1;
}

bool fb_sel_has(const FileBrowser *fb, i32 idx) {
    if (!fb || idx < 0 || idx >= fb->entry_count) return false;
    return (fb->selection_set[idx >> 3] & (1u << (idx & 7))) != 0;
}

void fb_sel_set(FileBrowser *fb, i32 idx, bool on) {
    if (!fb || idx < 0 || idx >= fb->entry_count) return;
    u8 *byte = &fb->selection_set[idx >> 3];
    u8 mask  = (u8)(1u << (idx & 7));
    bool was = (*byte & mask) != 0;
    if (on && !was) { *byte = (u8)(*byte | mask);  fb->selection_count++; }
    if (!on && was) { *byte = (u8)(*byte & ~mask); fb->selection_count--; }
}

void fb_sel_toggle(FileBrowser *fb, i32 idx) {
    if (!fb || idx < 0 || idx >= fb->entry_count) return;
    fb_sel_set(fb, idx, !fb_sel_has(fb, idx));
}

void fb_sel_range(FileBrowser *fb, i32 a, i32 b) {
    if (!fb) return;
    if (a > b) { i32 t = a; a = b; b = t; }
    if (a < 0) a = 0;
    if (b >= fb->entry_count) b = fb->entry_count - 1;
    for (i32 i = a; i <= b; i++) fb_sel_set(fb, i, true);
}

i32 fb_sel_collect(const FileBrowser *fb, i32 *out, i32 cap) {
    if (!fb || !out || cap <= 0) return 0;
    i32 n = 0;
    for (i32 i = 0; i < fb->entry_count && n < cap; i++) {
        if (fb->selection_set[i >> 3] & (1u << (i & 7))) {
            out[n++] = i;
            /* A selected directory already carries its selected descendants. */
            if (fb->entries[i].is_dir) {
                i32 depth = fb->entries[i].depth;
                while (i + 1 < fb->entry_count && fb->entries[i + 1].depth > depth) i++;
            }
        }
    }
    return n;
}

void fb_destroy(FileBrowser *fb) {
    if (!fb) return;
    fb_syn_invalidate(fb);
    fb_graph_job_drop(fb);     /* a scan in flight finishes on its own; nothing waits */
    fb_image_decode_cancel(fb);
    fb_sftp_list_cancel(fb);
    fb_local_list_cancel(fb);
    fb_tree_restore_clear(fb);
    fb_md_parse_cancel(fb);
    fb_image_state_reset(fb);
    fb_entries_reset(fb);
    if (fb->stat_id) fb_stat_forget(fb->stat_id);
    free(fb->entries);
    free(fb->view_content);
    if (fb->md_images) { md_image_cache_destroy(fb->md_images); fb->md_images = NULL; }
    if (fb->md_embeds) { md_embed_cache_destroy(fb->md_embeds); fb->md_embeds = NULL; }
    if (fb->md_arena.base) { arena_destroy(&fb->md_arena); }
    fb_md_parse_cancel(fb); free(fb->md_src); fb->md_src = NULL;
    fb->md_doc = NULL;
    free(fb->md_link_rects);
    fb->md_link_rects = NULL;
    fb->md_link_count = 0;
    free(fb->md_task_rects);
    fb->md_task_rects = NULL;
    free(fb->md_fold_rects);
    fb->md_fold_rects = NULL;
    fb->md_fold_count = 0;
    fb->md_task_count = 0;
    free(fb->md_glyph_rects);
    fb->md_glyph_rects = NULL;
    fb->md_glyph_count = 0;
    free(fb->md_outline);
    fb->md_outline = NULL;
    fb->md_outline_count = 0;
    free(fb->find_matches);
    fb->find_matches = NULL;
    fb->find_match_count = fb->find_match_cap = 0;
    free(fb->ac_items);
    fb->ac_items = NULL;
    fb->ac_count = fb->ac_cap = 0;
    if (fb->view_image_rgba) { stbi_image_free(fb->view_image_rgba); fb->view_image_rgba = NULL; }
    free(fb->view_image_frame_delays);
    /* Free undo frames + baseline mirrors. */
    for (i32 i = 0; i < fb->undo_count; i++) {
        free(fb->undo_frames[i].old_bytes);
        free(fb->undo_frames[i].new_bytes);
    }
    undo_pending_free(&fb->undo_pending);
    /* The knowledge graph is otherwise only freed by fb_close_viewer; a tab
     * closed while still in graph mode would leak the whole MdGraph (~2 MB). */
    if (fb->graph) { md_graph_free(fb->graph); fb->graph = NULL; }
    free(fb->graph_lbl_grid);  fb->graph_lbl_grid = NULL;
    free(fb->graph_lbl_key);   fb->graph_lbl_key = NULL;
    free(fb->graph_nbr_set);   fb->graph_nbr_set = NULL;
    memset(fb, 0, sizeof(*fb));
}

/* =========================================================================
 * Helpers
 * ========================================================================= */

const char *fb_format_size(u64 bytes) {
    static char buf[32];
    if (bytes < 1024) snprintf(buf, sizeof(buf), "%lluB", (unsigned long long)bytes);
    else if (bytes < 1024*1024) snprintf(buf, sizeof(buf), "%.1fK", bytes/1024.0);
    else if (bytes < 1024*1024*1024) snprintf(buf, sizeof(buf), "%.1fM", bytes/(1024.0*1024));
    else snprintf(buf, sizeof(buf), "%.1fG", bytes/(1024.0*1024*1024));
    return buf;
}

const char *fb_file_icon(const char *name, bool is_dir) {
    if (is_dir) return "[D]";
    const char *ext = strrchr(name, '.');
    if (!ext) return "   ";
    if (strcmp(ext, ".c") == 0 || strcmp(ext, ".h") == 0) return "[C]";
    if (strcmp(ext, ".py") == 0) return "[P]";
    if (strcmp(ext, ".js") == 0 || strcmp(ext, ".ts") == 0) return "[J]";
    if (strcmp(ext, ".rs") == 0) return "[R]";
    if (strcmp(ext, ".go") == 0) return "[G]";
    if (strcmp(ext, ".md") == 0) return "[M]";
    if (strcmp(ext, ".sh") == 0) return "[S]";
    if (strcmp(ext, ".txt") == 0) return "[T]";
    if (strcmp(ext, ".json") == 0) return "[J]";
    if (strcmp(ext, ".yaml") == 0 || strcmp(ext, ".yml") == 0) return "[Y]";
    if (strcmp(ext, ".toml") == 0) return "[T]";
    if (strcmp(ext, ".png") == 0 || strcmp(ext, ".jpg") == 0) return "[I]";
    return "   ";
}

/* Human-readable language for the editor status bar.
 *
 * Deliberately separate from fb_detect_mode(): that one answers "which viewer
 * renders this", while several distinct languages share FVIEW_CODE and each
 * wants its own name on the bar. The name itself comes from the scanner's
 * extension table (syntax_lang_label) — the same rows that pick the lexer —
 * so ".astro" cannot be highlighted as Astro and labelled "ASTRO", which is
 * what happened while this function carried a table of its own. Falls back to
 * the uppercased extension so a file type nobody enumerated still reports
 * something truer than "Text". */
const char *fb_language_label(const char *path) {
    if (!path || !*path) return "Text";
    const char *known = syntax_lang_label(path, strlen(path));
    if (known) return known;

    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    const char *ext = strrchr(base, '.');
    if (!ext || !ext[1]) return "Text";

    /* Unknown extension: ".foobar" → "FOOBAR", bounded and ASCII-only so a
     * crafted filename can't smuggle anything into the status bar. */
    static char fallback[12];
    usize n = 0;
    for (const char *p = ext + 1; *p && n + 1 < sizeof fallback; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 'a' || c > 'z') {
            if (c < 'A' || c > 'Z') { if (c < '0' || c > '9') return "Text"; }
        } else {
            c = (unsigned char)(c - 'a' + 'A');
        }
        fallback[n++] = (char)c;
    }
    if (n == 0) return "Text";
    fallback[n] = '\0';
    return fallback;
}

/* Line-ending style of the loaded buffer, for the status bar. Reports what
 * dominates rather than what appears first — a file that is overwhelmingly
 * LF with one stray CRLF is an LF file with a defect, and labelling it CRLF
 * would send the reader looking in the wrong place. */
const char *fb_eol_label(const char *content, usize len) {
    /* The rule is editor_detect_crlf's (a bounded scan of what dominates),
     * because it is also what decides which break Enter inserts: the bar
     * cannot name one ending while the editor writes the other. */
    return editor_detect_crlf(content, len) ? "CRLF" : "LF";
}

/* What the text's bytes are, for the status bar and the painter. Read again
 * only after the text changed (view_edit_gen), never per frame. */
static EditorEncoding fb_view_encoding(FileBrowser *fb) {
    if (fb->view_enc_for != fb->view_edit_gen + 1u) {
        fb->view_enc = (u8)editor_detect_encoding(fb->view_content, fb->view_size);
        fb->view_enc_for = fb->view_edit_gen + 1u;
    }
    return (EditorEncoding)fb->view_enc;
}

bool fb_path_is_svg(const char *path) {
    const char *ext = path ? strrchr(path, '.') : NULL;
    return ext && strcasecmp(ext, ".svg") == 0;
}

FileViewMode fb_detect_mode(const char *filename) {
    const char *ext = strrchr(filename, '.');
    if (ext && (strcmp(ext, ".md") == 0 || strcmp(ext, ".markdown") == 0))
        return FVIEW_MARKDOWN;
    /* An SVG is a drawing first and markup second: it opens drawn, and the
     * title bar switches to its source. The scanner below would call it code. */
    if (fb_path_is_svg(filename))
        return FVIEW_IMAGE;
    /* Code is whatever the scanner has a row for — by extension OR by
     * basename (Makefile, Dockerfile, go.mod), which is why this does not
     * early-out on a missing extension. */
    if (syntax_lang_for_path(filename, strlen(filename)) != SYN_LANG_NONE)
        return FVIEW_CODE;
    if (!ext) return FVIEW_TEXT;
    if (strcmp(ext, ".png")  == 0 || strcmp(ext, ".jpg")  == 0 ||
        strcmp(ext, ".jpeg") == 0 || strcmp(ext, ".gif")  == 0 ||
        strcmp(ext, ".bmp")  == 0 || strcmp(ext, ".tga")  == 0 ||
        strcmp(ext, ".psd")  == 0 || strcmp(ext, ".pnm")  == 0 ||
        strcmp(ext, ".ppm")  == 0 || strcmp(ext, ".pgm")  == 0 ||
        strcmp(ext, ".webp") == 0 || strcmp(ext, ".hdr")  == 0)
        return FVIEW_IMAGE;
    return FVIEW_TEXT;
}

void fb_entry_path(const FileBrowser *fb, const FileEntry *entry, char *out, usize out_size) {
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (!fb || !entry || !entry->name) return;
    if (entry->path) {
        if (strlen(entry->path) < out_size) memcpy(out, entry->path, strlen(entry->path) + 1);
        return;
    }

    if (strcmp(entry->name, "..") == 0) {
        if (strcmp(fb->cwd, "/") == 0) {
            snprintf(out, out_size, "/");
        } else {
            snprintf(out, out_size, "%s/..", fb->cwd);
        }
        return;
    }

    if (strcmp(fb->cwd, "/") == 0) {
        snprintf(out, out_size, "/%s", entry->name);
    } else {
        snprintf(out, out_size, "%s/%s", fb->cwd, entry->name);
    }
}

static int entry_cmp(const void *a, const void *b) {
    const FileEntry *ea = (const FileEntry *)a;
    const FileEntry *eb = (const FileEntry *)b;
    /* Dirs first, then alphabetical */
    if (ea->is_dir != eb->is_dir) return ea->is_dir ? -1 : 1;
    const char *na = ea->name ? ea->name : "";
    const char *nb = eb->name ? eb->name : "";
    return strcasecmp(na, nb);
}


/* Visible tree rows remain a flat, virtualised list. Only expanded directories
 * own descendants; closing a branch releases its rows and their path storage. */
struct FbTreeRestore {
    i32 count;
    char *paths[];
};

static void fb_tree_restore_clear(FileBrowser *fb) {
    if (!fb || !fb->tree_restore) return;
    for (i32 i = 0; i < fb->tree_restore->count; i++) free(fb->tree_restore->paths[i]);
    free(fb->tree_restore);
    fb->tree_restore = NULL;
}

static void fb_tree_restore_capture(FileBrowser *fb) {
    i32 cap = fb->entry_count + (fb->tree_restore ? fb->tree_restore->count : 0);
    if (cap > FB_MAX_ENTRIES) cap = FB_MAX_ENTRIES;
    struct FbTreeRestore *next = calloc(1, sizeof(*next) + (usize)cap * sizeof(char *));
    if (!next) return;
    /* Include not-yet-restored descendants if refresh is pressed mid-listing. */
    if (fb->tree_restore) {
        for (i32 i = 0; i < fb->tree_restore->count && next->count < cap; i++) {
            const char *path = fb->tree_restore->paths[i];
            if (path) {
                char *copy = strdup(path);
                if (copy) next->paths[next->count++] = copy;
            }
        }
    }
    for (i32 i = 0; i < fb->entry_count && next->count < cap; i++) {
        if (!fb->entries[i].expanded) continue;
        char path[FB_MAX_PATH];
        fb_entry_path(fb, &fb->entries[i], path, sizeof path);
        if (!path[0]) continue;
        bool duplicate = false;
        for (i32 j = 0; j < next->count; j++) duplicate |= !strcmp(path, next->paths[j]);
        char *copy = duplicate ? NULL : strdup(path);
        if (copy) next->paths[next->count++] = copy;
    }
    fb_tree_restore_clear(fb);
    if (next->count) fb->tree_restore = next;
    else free(next);
}

static void fb_tree_restore_apply(FileBrowser *fb) {
    if (!fb->tree_restore) return;
    for (i32 i = 0; i < fb->entry_count; i++) {
        FileEntry *e = &fb->entries[i];
        if (!e->is_dir || e->expanded || !strcmp(e->name, "..")) continue;
        char path[FB_MAX_PATH];
        fb_entry_path(fb, e, path, sizeof path);
        for (i32 j = 0; j < fb->tree_restore->count; j++) {
            char *saved = fb->tree_restore->paths[j];
            if (saved && !strcmp(saved, path)) {
                e->expanded = e->tree_pending = true;
                free(saved); fb->tree_restore->paths[j] = NULL;
                break;
            }
        }
    }
}

static void fb_tree_rows_changed(FileBrowser *fb, i32 start, i32 removed, i32 added) {
    u8 selected[sizeof fb->selection_set];
    memcpy(selected, fb->selection_set, sizeof selected);
    i32 old_count = fb->entry_count - added + removed;
    i32 primary = fb->selected, anchor = fb->selection_anchor;
    fb_sel_clear(fb);
    for (i32 i = 0; i < old_count; i++) {
        if (!(selected[i / 8] & (1u << (i % 8)))) continue;
        if (i >= start && i < start + removed) continue;
        i32 to = i < start ? i : i - removed + added;
        fb_sel_set(fb, to, true);
    }
    fb->selected = primary < start ? primary :
                   primary < start + removed ? start - 1 : primary - removed + added;
    fb->selection_anchor = anchor < start ? anchor :
                           anchor < start + removed ? start - 1 : anchor - removed + added;
    if (primary >= start && primary < start + removed) fb_sel_set(fb, start - 1, true);
    fb->hover_entry = -1;
    /* Stat results are indexed by row: no old result can land on a shifted row. */
    fb_stat_listing_changed(fb);
    for (i32 i = 0; i < fb->entry_count; i++) fb->entries[i].stat_pending = false;
    if (old_count > 0) fb->content_height *= (f32)fb->entry_count / old_count;
}

static i32 fb_tree_find_parent(const FileBrowser *fb, const char *path) {
    for (i32 i = 0; i < fb->entry_count; i++) {
        const FileEntry *e = &fb->entries[i];
        if (!e->is_dir || !e->expanded || !e->tree_pending) continue;
        char full[FB_MAX_PATH];
        fb_entry_path(fb, e, full, sizeof full);
        if (!strcmp(full, path)) return i;
    }
    return -1;
}

/* Transfer a job's children into their parent's visible branch. Names and
 * paths change owner only after every required allocation has succeeded. */
static bool fb_tree_install(FileBrowser *fb, const char *path, FileEntry *rows,
                            i32 count, bool opened) {
    i32 parent = fb_tree_find_parent(fb, path);
    if (parent < 0) return false;
    FileEntry *e = &fb->entries[parent];
    e->tree_pending = false;
    if (!opened) { e->expanded = false; return false; }
    i32 available = FB_MAX_ENTRIES - fb->entry_count, added = 0;
    for (i32 i = 0; i < count && added < available; i++) {
        if (!rows[i].name || !strcmp(rows[i].name, "..")) continue;
        if (!rows[i].path) {
            char full[FB_MAX_PATH];
            int n = snprintf(full, sizeof full, "%s%s%s", path,
                             !strcmp(path, "/") ? "" : "/", rows[i].name);
            if (n < 0 || (usize)n >= sizeof full) continue;
            rows[i].path = strdup(full);
        }
        if (!rows[i].path) { e->expanded = false; return false; }
        rows[i].depth = e->depth + 1;
        added++;
    }
    i32 total = fb->entry_count + added;
    if (total > fb->entry_cap) {
        i32 cap = fb->entry_cap ? fb->entry_cap * 2 : 64;
        if (cap < total) cap = total;
        if (cap > FB_MAX_ENTRIES) cap = FB_MAX_ENTRIES;
        FileEntry *grown = realloc(fb->entries, (usize)cap * sizeof(FileEntry));
        if (!grown) { fb->entries[parent].expanded = false; return false; }
        fb->entries = grown; fb->entry_cap = cap;
    }
    i32 start = parent + 1;
    memmove(fb->entries + start + added, fb->entries + start,
            (usize)(fb->entry_count - start) * sizeof(FileEntry));
    i32 written = 0;
    for (i32 i = 0; i < count && written < added; i++) {
        if (!rows[i].path) continue;
        fb->entries[start + written++] = rows[i];
        rows[i].name = rows[i].path = NULL;
    }
    fb->entry_count = total;
    fb_tree_rows_changed(fb, start, 0, added);
    fb_tree_restore_apply(fb);
    return true;
}

void fb_operation_dir(const FileBrowser *fb, char *out, usize cap) {
    if (!out || !cap) return;
    out[0] = '\0';
    if (!fb) return;
    if (fb->selected >= 0 && fb->selected < fb->entry_count) {
        const FileEntry *e = &fb->entries[fb->selected];
        if (strcmp(e->name, "..")) {
            fb_entry_path(fb, e, out, cap);
            if (e->is_dir) return;
            char *slash = strrchr(out, '/');
            if (slash == out) out[1] = '\0';
            else if (slash) *slash = '\0';
            return;
        }
    }
    if (strlen(fb->cwd) < cap) memcpy(out, fb->cwd, strlen(fb->cwd) + 1);
}

/* =========================================================================
 * Local filesystem navigation
 * ========================================================================= */

/* =========================================================================
 * Local metadata off the draw path
 *
 * The size column is the only consumer of a local entry's stat, and a stat ON
 * THE DRAW PATH would park the frame for as long as lstat takes, per visible
 * row, on a slow mount, a cold disk or an unreachable share. One process-wide
 * worker takes the requests; the draw queues what it is looking at and moves
 * on, and fb_stat_poll hands the answers back the next frame.
 *
 * What holds it together: a result names its browser (stat_id, never reused)
 * and its listing generation (stat_gen, bumped by every navigate), and is
 * dropped when either has moved on — a row index means nothing across a
 * refresh. A navigate purges the browser's queued requests, so the worker
 * does not stat a directory the user has left; a queue that is full drops the
 * newest request (the row is drawn again next frame and asks again). A
 * failed stat is delivered as "known, empty" so it is never asked twice. The
 * thread exits after a few idle seconds and is re-made on demand, so a
 * browser that shows a local listing once does not keep a thread for the
 * life of the process. SFTP listings arrive filled and never come here.
 * ========================================================================= */

#define FB_STAT_QUEUE_CAP    512       /* requests, process-wide */
#define FB_STAT_RESULT_CAP   1024      /* results waiting for a poll */
#define FB_STAT_PER_FB_CAP   160       /* a browser's queued rows: ~4 screens */
#define FB_STAT_IDLE_EXIT_SEC 4.0

typedef struct {
    u64  fb_id;
    u32  gen;
    i32  index;
    char *path;                        /* heap, owned by the request */
} FbStatReq;

typedef struct {
    u64  fb_id;
    u32  gen;
    i32  index;
    bool ok;
    u64  size, mtime;
    u32  mode;
} FbStatRes;

static struct {
    pthread_mutex_t mtx;
    pthread_cond_t  cv;
    pthread_t       tid;
    bool            live;              /* thread running */
    bool            stop;
    bool            in_flight;         /* a request is being stat'ed right now */
    bool            in_flight_cancelled;
    u64             in_flight_id;
    u32             in_flight_gen;
    FbStatReq       reqs[FB_STAT_QUEUE_CAP];
    i32             req_head, req_count;
    FbStatRes       res[FB_STAT_RESULT_CAP];
    i32             res_count;
    _Atomic bool    results_ready;
    u64             next_id;
    u64             queued, done, dropped;
    /* LIU_FB_STAT_DELAY_MS adds a per-request delay in the worker when set.
     * The value is read once. */
    i32             delay_ms;
    bool            delay_read;
} g_stat = { .mtx = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER, .next_id = 1 };

static void stat_req_free(FbStatReq *q) { free(q->path); q->path = NULL; }

static void *fb_stat_worker(void *arg) {
    (void)arg;
    pthread_mutex_lock(&g_stat.mtx);
    for (;;) {
        while (!g_stat.stop && g_stat.req_count == 0) {
            struct timespec until;
            clock_gettime(CLOCK_REALTIME, &until);
            until.tv_sec += (time_t)FB_STAT_IDLE_EXIT_SEC;
            int rc = pthread_cond_timedwait(&g_stat.cv, &g_stat.mtx, &until);
            if (rc == ETIMEDOUT && g_stat.req_count == 0) {
                /* Idle: leave. The next request makes a new thread. */
                g_stat.live = false;
                pthread_mutex_unlock(&g_stat.mtx);
                return NULL;
            }
        }
        if (g_stat.stop) { g_stat.live = false; pthread_mutex_unlock(&g_stat.mtx); return NULL; }
        FbStatReq q = g_stat.reqs[g_stat.req_head];
        g_stat.reqs[g_stat.req_head].path = NULL;
        g_stat.req_head = (g_stat.req_head + 1) % FB_STAT_QUEUE_CAP;
        g_stat.req_count--;
        g_stat.in_flight = true;
        g_stat.in_flight_cancelled = false;
        g_stat.in_flight_id = q.fb_id;
        g_stat.in_flight_gen = q.gen;
        i32 delay = g_stat.delay_ms;
        pthread_mutex_unlock(&g_stat.mtx);

        if (delay > 0) {
            struct timespec ts = { delay / 1000, (delay % 1000) * 1000000L };
            nanosleep(&ts, NULL);
        }
        FbStatRes r = { q.fb_id, q.gen, q.index, false, 0, 0, 0 };
        struct stat st;
        if (q.path && lstat(q.path, &st) == 0) {
            r.ok = true;
            r.size = (u64)st.st_size;
            r.mode = (u32)st.st_mode;
            r.mtime = (u64)st.st_mtime;
        }
        stat_req_free(&q);

        pthread_mutex_lock(&g_stat.mtx);
        g_stat.in_flight = false;
        if (!g_stat.in_flight_cancelled && !g_stat.stop)
            g_stat.res[g_stat.res_count++] = r; /* slot reserved at admission */
        else g_stat.dropped++;
        g_stat.done++;
        atomic_store_explicit(&g_stat.results_ready, g_stat.res_count > 0, memory_order_release);
        pthread_mutex_unlock(&g_stat.mtx);
        platform_wake_event_loop();
        pthread_mutex_lock(&g_stat.mtx);
    }
}

/* Under the mutex. */
static bool fb_stat_thread_ensure(void) {
    if (g_stat.live) return true;
    g_stat.stop = false;
    if (pthread_create(&g_stat.tid, NULL, fb_stat_worker, NULL) != 0) return false;
    pthread_detach(g_stat.tid);          /* it leaves on its own when idle */
    g_stat.live = true;
    return true;
}

/* Drop every queued request and waiting result that names this browser and
 * generation (or every generation with gen == 0). Under the mutex. */
static void fb_stat_purge_locked(u64 fb_id, u32 keep_gen) {
    if (g_stat.in_flight && g_stat.in_flight_id == fb_id &&
        (keep_gen == 0 || g_stat.in_flight_gen != keep_gen))
        g_stat.in_flight_cancelled = true;
    i32 kept = 0, head = g_stat.req_head;
    for (i32 i = 0; i < g_stat.req_count; i++) {
        i32 at = (head + i) % FB_STAT_QUEUE_CAP;
        FbStatReq q = g_stat.reqs[at];
        bool drop = q.fb_id == fb_id && (keep_gen == 0 || q.gen != keep_gen);
        if (drop) { stat_req_free(&q); g_stat.dropped++; continue; }
        g_stat.reqs[(head + kept) % FB_STAT_QUEUE_CAP] = q;
        kept++;
    }
    /* Slots past `kept` may hold stale copies of moved requests: clear them. */
    for (i32 i = kept; i < g_stat.req_count; i++)
        g_stat.reqs[(head + i) % FB_STAT_QUEUE_CAP].path = NULL;
    g_stat.req_count = kept;
    i32 rk = 0;
    for (i32 i = 0; i < g_stat.res_count; i++) {
        FbStatRes r = g_stat.res[i];
        if (r.fb_id == fb_id && (keep_gen == 0 || r.gen != keep_gen)) continue;
        g_stat.res[rk++] = r;
    }
    g_stat.res_count = rk;
    atomic_store_explicit(&g_stat.results_ready, rk > 0, memory_order_release);
}

/* Queue a stat for one entry, once. The draw path calls this for every row it
 * paints, so it must be cheap and must never block: no I/O here. */
void fb_entry_ensure_stat(FileBrowser *fb, FileEntry *e) {
    if (!fb || !e || e->stat_known || e->stat_pending || !e->name) return;
    if (fb->source != FB_SOURCE_LOCAL) { e->stat_known = true; return; }   /* SFTP rows arrive filled */
    /* A browser that was never fb_init'ed (a test's calloc) gets its identity
     * at its first request; without one no result could ever come back. */
    if (!fb->stat_id) { fb->stat_id = fb_stat_new_id(); fb->stat_gen = 1; }
    if (!fb->entries || e < fb->entries || e >= fb->entries + fb->entry_count) return;
    i32 index = (i32)(e - fb->entries);
    /* fb_entry_path is the one place that knows how an entry maps to a path;
     * open-coding cwd + "/" + name here is how the two would drift. */
    char full[FB_MAX_PATH * 2];
    fb_entry_path(fb, e, full, sizeof full);
    if (!full[0]) { e->stat_known = true; return; }
    char *path = strdup(full);
    if (!path) return;

    pthread_mutex_lock(&g_stat.mtx);
    if (!g_stat.delay_read) {
        const char *d = liu_dev_env("LIU_FB_STAT_DELAY_MS");
        g_stat.delay_ms = (d && *d) ? atoi(d) : 0;
        g_stat.delay_read = true;
    }
    /* Too many rows of this browser already waiting — a fast scroll — the
     * oldest are the ones that left the screen first: drop them. */
    i32 mine = 0;
    for (i32 i = 0; i < g_stat.req_count; i++)
        mine += g_stat.reqs[(g_stat.req_head + i) % FB_STAT_QUEUE_CAP].fb_id == fb->stat_id;
    if (mine >= FB_STAT_PER_FB_CAP) {
        i32 drop = FB_STAT_PER_FB_CAP / 2, kept = 0, head = g_stat.req_head;
        for (i32 i = 0; i < g_stat.req_count; i++) {
            i32 at = (head + i) % FB_STAT_QUEUE_CAP;
            FbStatReq q = g_stat.reqs[at];
            if (drop > 0 && q.fb_id == fb->stat_id) {
                /* The row will ask again if it is still on screen. */
                if (q.index >= 0 && q.index < fb->entry_count && q.gen == fb->stat_gen)
                    fb->entries[q.index].stat_pending = false;
                stat_req_free(&q); g_stat.dropped++; drop--;
                continue;
            }
            g_stat.reqs[(head + kept) % FB_STAT_QUEUE_CAP] = q;
            kept++;
        }
        for (i32 i = kept; i < g_stat.req_count; i++)
            g_stat.reqs[(head + i) % FB_STAT_QUEUE_CAP].path = NULL;
        g_stat.req_count = kept;
    }
    /* Reserve a result slot at admission: a finished result that had to be
     * dropped would leave its row stat_pending forever, because the worker
     * cannot touch the owner to clear that flag. Refused requests retain no
     * pending flag and retry. */
    if (g_stat.req_count >= FB_STAT_QUEUE_CAP ||
        g_stat.req_count + g_stat.res_count + (g_stat.in_flight ? 1 : 0) >= FB_STAT_RESULT_CAP ||
        !fb_stat_thread_ensure()) {
        pthread_mutex_unlock(&g_stat.mtx);
        free(path);
        return;                          /* full: the row asks again next frame */
    }
    i32 at = (g_stat.req_head + g_stat.req_count) % FB_STAT_QUEUE_CAP;
    g_stat.reqs[at] = (FbStatReq){ fb->stat_id, fb->stat_gen, index, path };
    g_stat.req_count++;
    g_stat.queued++;
    e->stat_pending = true;
    pthread_cond_signal(&g_stat.cv);
    pthread_mutex_unlock(&g_stat.mtx);
}

bool fb_stat_poll(FileBrowser *fb) {
    if (!fb || !fb->stat_id ||
        !atomic_load_explicit(&g_stat.results_ready, memory_order_acquire)) return false;
    bool changed = false;
    pthread_mutex_lock(&g_stat.mtx);
    if (g_stat.res_count == 0) { pthread_mutex_unlock(&g_stat.mtx); return false; }
    i32 kept = 0;
    for (i32 i = 0; i < g_stat.res_count; i++) {
        FbStatRes r = g_stat.res[i];
        if (r.fb_id != fb->stat_id) { g_stat.res[kept++] = r; continue; }
        if (r.gen != fb->stat_gen) continue;                 /* a listing since left */
        if (r.index < 0 || r.index >= fb->entry_count) continue;
        FileEntry *e = &fb->entries[r.index];
        if (!e->name || e->stat_known) continue;
        e->stat_pending = false;
        e->stat_known = true;                                 /* failure included */
        if (r.ok) {
            e->size        = r.size;
            e->permissions = r.mode;
            e->mtime       = r.mtime;
        }
        changed = true;
    }
    g_stat.res_count = kept;
    atomic_store_explicit(&g_stat.results_ready, kept > 0, memory_order_release);
    pthread_mutex_unlock(&g_stat.mtx);
    return changed;
}

void fb_stat_counters(u64 *queued, u64 *done, u64 *dropped) {
    pthread_mutex_lock(&g_stat.mtx);
    if (queued)  *queued  = g_stat.queued;
    if (done)    *done    = g_stat.done;
    if (dropped) *dropped = g_stat.dropped;
    pthread_mutex_unlock(&g_stat.mtx);
}

/* A listing was replaced (navigate/refresh/reset): nothing queued or waiting
 * for it is worth anything now. */
static void fb_stat_listing_changed(FileBrowser *fb) {
    if (!fb || !fb->stat_id) return;
    fb->stat_gen++;
    pthread_mutex_lock(&g_stat.mtx);
    fb_stat_purge_locked(fb->stat_id, fb->stat_gen);
    pthread_mutex_unlock(&g_stat.mtx);
}

static u64 fb_stat_new_id(void) {
    pthread_mutex_lock(&g_stat.mtx);
    u64 id = g_stat.next_id++;
    pthread_mutex_unlock(&g_stat.mtx);
    return id;
}

/* A browser is gone: nothing queued or waiting for any of its listings may
 * be delivered, and its id is never handed out again. */
static void fb_stat_forget(u64 fb_id) {
    pthread_mutex_lock(&g_stat.mtx);
    fb_stat_purge_locked(fb_id, 0);
    pthread_mutex_unlock(&g_stat.mtx);
}

void fb_stat_shutdown(void) {
    pthread_mutex_lock(&g_stat.mtx);
    g_stat.stop = true;
    pthread_cond_broadcast(&g_stat.cv);
    /* The thread is detached and leaves on `stop`; wait for the request in
     * flight to be handed back so its path is not freed under it. */
    while (g_stat.live) {
        pthread_mutex_unlock(&g_stat.mtx);
        struct timespec ts = { 0, 1000000L };
        nanosleep(&ts, NULL);
        pthread_mutex_lock(&g_stat.mtx);
    }
    for (i32 i = 0; i < g_stat.req_count; i++)
        stat_req_free(&g_stat.reqs[(g_stat.req_head + i) % FB_STAT_QUEUE_CAP]);
    g_stat.req_count = g_stat.req_head = 0;
    g_stat.res_count = 0;
    atomic_store_explicit(&g_stat.results_ready, false, memory_order_release);
    g_stat.stop = false;
    pthread_mutex_unlock(&g_stat.mtx);
}

/* ---- asynchronous local listing ---------------------------------------
 *
 * opendir and the readdir walk are round trips on a network mount, and a dead
 * or sleeping one does not answer at all. On the thread that paints, a click
 * into an unreachable share — or the sidebar following the shell there —
 * would freeze the window for as long as the kernel takes to give up.
 *
 * The walk (opendir, readdir, the rare DT_UNKNOWN lstat, NFC, the sort) is a
 * job on a detached worker, owned jointly by the browser and the worker like
 * the SFTP listing below. fb_navigate never waits by default: it commits the
 * navigation request and callers consume rows through fb_poll_async. The
 * explicit timing hook can opt into a wait for a caller that needs the rows
 * at once. While a listing is pending the browser moves to the folder (a
 * refresh of the same folder keeps showing the old rows), the rows are
 * installed by fb_poll_async when the worker publishes, and a late failure
 * leaves the folder empty — the SFTP listing's answer to the same case.
 *
 * Workers stuck in a dead mount cannot be interrupted, so the number alive is
 * capped: past FB_LOCAL_LIST_MAX_LIVE a navigate is refused (returns false)
 * rather than stacking another thread onto the same hung mount. */

#define FB_LOCAL_LIST_WAIT_MS  0
#define FB_LOCAL_LIST_MAX_LIVE 8

typedef struct FbLocalListJob {
    pthread_t       thread;
    char            path[FB_MAX_PATH];     /* as asked, for opendir          */
    char            cwd[FB_MAX_PATH];      /* NFC, what fb->cwd becomes      */
    u64             generation;
    bool            tree;
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    bool            finished;              /* under mu, for the timed wait   */
    _Atomic bool    done;                  /* lock-free view for the poll    */
    _Atomic bool    cancel;
    _Atomic i32     refs;
    bool            opened;                /* opendir succeeded              */
    i32             delay_ms;              /* test stand-in for a slow mount */
    FileEntry      *rows;                  /* names are heap, already NFC    */
    i32             count;
    bool            truncated;             /* the folder holds more than the list */
} FbLocalListJob;

static _Atomic i32 g_fb_local_live;
static u64 g_fb_nav_generation;            /* shared with the SFTP listing */

i32 fb_local_list_live(void) {
    return atomic_load_explicit(&g_fb_local_live, memory_order_acquire);
}

/* Both tunables are resolved on the main thread (fb_navigate) and carried
 * in the job: a cached getenv read from the worker is a data race. */
static i32 g_fb_list_wait_ms  = -2;
static i32 g_fb_list_delay_ms = -2;

void fb_local_list_set_timing(i32 wait_ms, i32 delay_ms) {
    g_fb_list_wait_ms  = wait_ms  < 0 ? 0 : wait_ms;
    g_fb_list_delay_ms = delay_ms < 0 ? 0 : delay_ms;
}

static i32 fb_local_list_wait_ms(void) {
    if (g_fb_list_wait_ms == -2) {
        const char *e = liu_dev_env("LIU_FB_LIST_WAIT_MS");
        g_fb_list_wait_ms = (e && *e) ? atoi(e) : FB_LOCAL_LIST_WAIT_MS;
        if (g_fb_list_wait_ms < 0) g_fb_list_wait_ms = 0;
    }
    return g_fb_list_wait_ms;
}

/* Stands in for a slow mount (milliseconds before opendir). */
static i32 fb_local_list_delay_ms(void) {
    if (g_fb_list_delay_ms == -2) {
        const char *e = liu_dev_env("LIU_FB_LIST_DELAY_MS");
        g_fb_list_delay_ms = (e && *e) ? atoi(e) : 0;
        if (g_fb_list_delay_ms < 0) g_fb_list_delay_ms = 0;
    }
    return g_fb_list_delay_ms;
}

static void fb_local_rows_free(FileEntry *rows, i32 count) {
    for (i32 i = 0; i < count; i++) { free(rows[i].name); free(rows[i].path); }
    free(rows);
}

static void fb_local_list_release(FbLocalListJob *j) {
    if (!j) return;
    if (atomic_fetch_sub_explicit(&j->refs, 1, memory_order_acq_rel) != 1) return;
    fb_local_rows_free(j->rows, j->count);
    pthread_mutex_destroy(&j->mu);
    pthread_cond_destroy(&j->cv);
    free(j);
}

/* A heap copy of `src` in NFC — the same normalisation fb_entry_set_name
 * does, done here so the main thread does not repeat it per row. */
static char *fb_local_name_dup(const char *src) {
    char buf[1024];
    if (platform_utf8_normalize_nfc(src, buf, sizeof buf)) src = buf;
    usize n = strlen(src);
    char *out = malloc(n + 1);
    if (out) memcpy(out, src, n + 1);
    return out;
}

static void fb_local_list_run(FbLocalListJob *j) {
    i32 delay = j->delay_ms;
    if (delay > 0) {
        struct timespec ts = { delay / 1000, (long)(delay % 1000) * 1000000L };
        nanosleep(&ts, NULL);
    }
    if (atomic_load_explicit(&j->cancel, memory_order_acquire)) return;
    DIR *dir = opendir(j->path);
    if (!dir) return;
    j->opened = true;
    i32 cap = 0;
    struct dirent *de;
    while ((de = readdir(dir))) {
        if (atomic_load_explicit(&j->cancel, memory_order_acquire)) break;
        if (de->d_name[0] == '.' && de->d_name[1] == '\0') continue;
        if (j->count >= FB_MAX_ENTRIES) { j->truncated = true; break; }
        if (j->count >= cap) {
            i32 nc = cap ? cap * 2 : 64;
            if (nc > FB_MAX_ENTRIES) nc = FB_MAX_ENTRIES;
            FileEntry *grown = realloc(j->rows, (usize)nc * sizeof(FileEntry));
            if (!grown) break;
            j->rows = grown;
            cap = nc;
        }
        FileEntry *e = &j->rows[j->count];
        memset(e, 0, sizeof(*e));
        e->name = fb_local_name_dup(de->d_name);
        if (!e->name) break;
        /* Display names may be NFC; file operations must keep the disk bytes. */
        if (strcmp(de->d_name, "..")) {
            char full[FB_MAX_PATH];
            int n = snprintf(full, sizeof full, "%s%s%s", j->path,
                             !strcmp(j->path, "/") ? "" : "/", de->d_name);
            if (n < 0 || (usize)n >= sizeof full) {
                free(e->name); e->name = NULL; continue;
            }
            e->path = strdup(full);
            if (!e->path) { free(e->name); e->name = NULL; break; }
        }

        if (de->d_name[0] == '.' && de->d_name[1] == '.' && de->d_name[2] == '\0') {
            e->is_dir = true;
            e->stat_known = true;      /* nothing to show for ".." */
        } else if (de->d_type != DT_UNKNOWN) {
            /* readdir already answered the only two questions the LISTING has
             * to answer — is it a directory, is it a link — so do not stat.
             *
             * On a network mount a stat is a round trip per entry. Only the
             * size column needs the rest, and only for a row actually on
             * screen — fb_entry_ensure_stat fills it there, once.
             *
             * d_type also reports the link itself, where stat() FOLLOWS a
             * symlink, so S_ISLNK on its result could never be true. */
            e->is_dir     = (de->d_type == DT_DIR);
            e->is_symlink = (de->d_type == DT_LNK);
            /* ...and that is the one question d_type cannot answer: what a
             * LINK is a link to. A link to a folder (/tmp, /var and /etc on
             * macOS, each package of a pnpm node_modules) has to be entered,
             * not read as text, so a link, and only a link, is asked what it
             * points at (one stat that follows it, relative to the directory
             * already open). A dangling one stays a file. */
            if (e->is_symlink) {
                struct stat target;
                if (fstatat(dirfd(dir), de->d_name, &target, 0) == 0)
                    e->is_dir = S_ISDIR(target.st_mode);
            }
        } else {
            /* A filesystem that does not fill d_type. Rare, and the stat is
             * the only way to know — take everything from it. */
            struct stat st;
            char full_path[FB_MAX_PATH * 2];
            if (strcmp(j->path, "/") == 0)
                snprintf(full_path, sizeof(full_path), "/%s", de->d_name);
            else
                snprintf(full_path, sizeof(full_path), "%s/%s", j->path, de->d_name);
            if (lstat(full_path, &st) == 0) {
                e->size = (u64)st.st_size;
                e->permissions = (u32)st.st_mode;
                e->mtime = (u64)st.st_mtime;
                e->is_dir = S_ISDIR(st.st_mode);
                e->is_symlink = S_ISLNK(st.st_mode);
                e->stat_known = true;
                /* The same question as above: what the link points at. */
                struct stat target;
                if (e->is_symlink && stat(full_path, &target) == 0)
                    e->is_dir = S_ISDIR(target.st_mode);
            }
            /* lstat failed and d_type is unknown: leave it a zero-size file. */
        }
        j->count++;
    }
    closedir(dir);

    /* Sort: dirs first, then alpha — ".." stays on top. */
    if (j->count > 1) {
        i32 start = 0;
        for (i32 i = 0; i < j->count; i++) {
            if (j->rows[i].name && strcmp(j->rows[i].name, "..") == 0) {
                if (i != 0) { FileEntry t = j->rows[0]; j->rows[0] = j->rows[i]; j->rows[i] = t; }
                start = 1;
                break;
            }
        }
        qsort(j->rows + start, (usize)(j->count - start), sizeof(FileEntry), entry_cmp);
    }
}

static void fb_local_list_publish(FbLocalListJob *j) {
    pthread_mutex_lock(&j->mu);
    j->finished = true;
    atomic_store_explicit(&j->done, true, memory_order_release);
    pthread_cond_broadcast(&j->cv);
    pthread_mutex_unlock(&j->mu);
}

static void *fb_local_list_worker(void *arg) {
    FbLocalListJob *j = (FbLocalListJob *)arg;
#ifdef __APPLE__
    pthread_setname_np("liu-fb-list");
#else
    pthread_setname_np(pthread_self(), "liu-fb-list");
#endif
    fb_local_list_run(j);
    fb_local_list_publish(j);
    /* Queued tree expansions must see the free admission slot on this wake. */
    atomic_fetch_sub_explicit(&g_fb_local_live, 1, memory_order_acq_rel);
    platform_wake_event_loop();
    fb_local_list_release(j);
    return NULL;
}

/* Give up on the listing in flight without waiting for it. */
static void fb_local_list_cancel(FileBrowser *fb) {
    if (!fb || !fb->local_list_job) return;
    FbLocalListJob *j = fb->local_list_job;
    fb->local_list_job = NULL;
    fb->pending_open_name[0] = '\0';
    fb->pending_open_edit = false;
    fb->pending_open_anchor[0] = '\0';
    atomic_store_explicit(&j->cancel, true, memory_order_release);
    fb_local_list_release(j);
}

/* Point the browser at the job's folder: the part of a navigate that does
 * not depend on the rows. `keep_rows` leaves the current entries in place
 * (a refresh of the folder already shown, still in flight). */
static void fb_local_list_commit(FileBrowser *fb, const FbLocalListJob *j,
                                 bool keep_rows) {
    fb->source = FB_SOURCE_LOCAL;
    snprintf(fb->cwd, sizeof(fb->cwd), "%s", j->cwd);
    if (!keep_rows) fb_entries_reset(fb);
    fb->md_only_entries = false;   /* full listing */
    fb->selected = -1;
    fb_sel_clear(fb);
    fb->scroll_offset = 0;
    fb->scroll_offset_px = 0.0f;
}

/* Move a finished job's rows into the browser. Names change owner. */
static void fb_local_list_install(FileBrowser *fb, FbLocalListJob *j) {
    if (j->tree) { fb_tree_install(fb, j->path, j->rows, j->count, j->opened); return; }
    fb_entries_reset(fb);
    fb->selected = -1;
    fb_sel_clear(fb);
    if (!j->opened || j->count == 0) return;
    if (fb->entry_cap < j->count || !fb->entries) {
        i32 want = j->count < 64 ? 64 : j->count;
        FileEntry *grown = realloc(fb->entries, (usize)want * sizeof(FileEntry));
        if (!grown) return;            /* rows stay with the job and are freed */
        fb->entries = grown;
        fb->entry_cap = want;
    }
    memcpy(fb->entries, j->rows, (usize)j->count * sizeof(FileEntry));
    fb->entry_count = j->count;
    free(j->rows);
    j->rows = NULL;
    j->count = 0;
    fb_tree_restore_apply(fb);
    /* A folder past the list's size shows the first rows readdir gave, in no
     * order the user can guess, sorted as though they were all of it: said. */
    if (j->truncated)
        fb_notice(fb, false, "This folder has more than %d items: only some of them are listed",
                  FB_MAX_ENTRIES - 1);
}

/* A file asked for by name while its folder's listing was in flight. */
static void fb_local_list_open_pending(FileBrowser *fb) {
    if (!fb->pending_open_name[0]) return;
    char name[sizeof fb->pending_open_name];
    snprintf(name, sizeof name, "%s", fb->pending_open_name);
    bool keep = fb->pending_open_keep_graph;
    bool edit = fb->pending_open_edit;
    char anchor[sizeof fb->pending_open_anchor];
    snprintf(anchor, sizeof anchor, "%s", fb->pending_open_anchor);
    bool is_block = fb->pending_open_anchor_is_block;
    fb->pending_open_name[0] = '\0';
    fb->pending_open_keep_graph = false;
    fb->pending_open_edit = false;
    fb->pending_open_anchor[0] = '\0';
    for (i32 i = 0; i < fb->entry_count; i++) {
        if (fb->entries[i].name && strcmp(fb->entries[i].name, name) == 0) {
            fb->selected = i;
            if (fb_open_file_edit(fb, i, edit)) {
                if (anchor[0]) fb_md_seek_anchor(fb, anchor, is_block);
            }
            if (keep && fb->graph && fb->view_mode != FVIEW_GRAPH)
                fb->graph_return = true;
            return;
        }
    }
    if (keep) fb->graph_return = true;
}

bool fb_local_listing_active(const FileBrowser *fb) {
    return fb && fb->local_list_job != NULL;
}

bool fb_navigate(FileBrowser *fb, const char *path) {
    if (!fb || !path || !path[0]) return false;
    if (fb_local_list_live() >= FB_LOCAL_LIST_MAX_LIVE) return false;
    if (fb->source == FB_SOURCE_LOCAL && !strcmp(fb->cwd, path)) fb_tree_restore_capture(fb);
    else fb_tree_restore_clear(fb);

    FbLocalListJob *j = calloc(1, sizeof *j);
    if (!j) return false;
    snprintf(j->path, sizeof j->path, "%s", path);
    /* Normalize to NFC for the same reason as fb_entry_set_name — the CWD is
     * rendered in the header strip with the same glyph pipeline, so an NFD
     * path would show base-char + invisible combining mark cells. Normalize
     * through the job, not fb->cwd: fb_refresh() passes fb->cwd AS `path`,
     * and platform_utf8_normalize_nfc clears dst[0] before reading src, so
     * an in-place call would erase the folder name. */
    if (!platform_utf8_normalize_nfc(path, j->cwd, sizeof j->cwd))
        snprintf(j->cwd, sizeof j->cwd, "%s", path);
    pthread_mutex_init(&j->mu, NULL);
    pthread_cond_init(&j->cv, NULL);
    atomic_init(&j->done, false);
    atomic_init(&j->cancel, false);
    atomic_init(&j->refs, 2);          /* this call/browser, and the worker */
    j->delay_ms = fb_local_list_delay_ms();

    atomic_fetch_add_explicit(&g_fb_local_live, 1, memory_order_acq_rel);
    if (pthread_create(&j->thread, NULL, fb_local_list_worker, j) != 0) {
        /* Admission failure must not move disk I/O back onto the UI thread. */
        atomic_fetch_sub_explicit(&g_fb_local_live, 1, memory_order_acq_rel);
        fb_local_list_release(j);
        fb_local_list_release(j);
        return false;
    } else {
        pthread_detach(j->thread);
    }

    /* Wait out the budget. The absolute deadline is on the realtime clock,
     * which is what pthread_cond_timedwait measures against. */
    bool finished = atomic_load_explicit(&j->done, memory_order_acquire);
    i32 ms = fb_local_list_wait_ms();
    if (!finished && ms > 0) {
        pthread_mutex_lock(&j->mu);
        struct timespec dl;
        clock_gettime(CLOCK_REALTIME, &dl);
        dl.tv_sec  += ms / 1000;
        dl.tv_nsec += (long)(ms % 1000) * 1000000L;
        if (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
        while (!j->finished) {
            if (pthread_cond_timedwait(&j->cv, &j->mu, &dl) == ETIMEDOUT) break;
        }
        finished = j->finished;
        pthread_mutex_unlock(&j->mu);
    }

    if (finished) {
        if (!j->opened) { fb_local_list_release(j); return false; }
        /* Whatever was in flight is for a folder the user has left. */
        fb_sftp_list_cancel(fb);
        fb_local_list_cancel(fb);
        fb_local_list_commit(fb, j, false);
        fb->nav_generation = ++g_fb_nav_generation;
        fb_local_list_install(fb, j);
        fb_local_list_release(j);
        fb_tree_start_pending(fb);
        return true;
    }

    /* Slow: commit to the folder now, install the rows when they land. A
     * refresh of the folder already on screen keeps showing its rows. */
    bool same = fb->source == FB_SOURCE_LOCAL && !fb->local_list_job &&
                !fb->sftp_list_job && strcmp(fb->cwd, j->cwd) == 0 &&
                fb->entry_count > 0;
    fb_sftp_list_cancel(fb);
    fb_local_list_cancel(fb);
    fb_local_list_commit(fb, j, same);
    fb->nav_generation = ++g_fb_nav_generation;
    j->generation = fb->nav_generation;
    fb->local_list_job = j;            /* the browser keeps this call's ref */
    return true;
}

/* =========================================================================
 * SFTP navigation
 * ========================================================================= */

/* ---- asynchronous remote listing -------------------------------------
 *
 * opendir + N readdirs are network round trips. Run on the main thread with
 * the session BLOCKING, they would freeze the window on a link with any real
 * latency for as long as the server takes — proportional to the directory
 * size, because libssh2 refills its readdir buffer over the wire.
 *
 * So the listing is a job, owned jointly by the browser and its worker in
 * the same shape as the image decode above: navigating away or closing the
 * browser drops a reference and returns, and a worker whose browser is gone
 * frees the job itself. Results carry the generation they were requested at,
 * so a listing that arrives after the user has moved on is discarded rather
 * than installed over wherever they went.
 *
 * The worker retains the Session for its lifetime: fb->sftp_handle belongs to
 * the Session, and session teardown frees it after the last reference. The
 * scope it takes is the one from ssh_session.h, which suspends the main
 * loop's polling of that session for exactly the window it holds it. */

typedef struct {
    char *name;          /* raw remote name; NFC normalisation is main-thread */
    u64   size;
    u32   permissions;
    u64   mtime;
    bool  is_dir;
    bool  is_symlink;
} FbSftpRow;

typedef struct FbSftpListJob {
    pthread_t     thread;
    Session      *session;       /* retained for the worker's lifetime */
    void         *sftp;          /* LIBSSH2_SFTP*, owned by the Session */
    char          path[FB_MAX_PATH];
    u64           generation;
    bool          tree;
    _Atomic bool  done;
    _Atomic bool  cancel;
    _Atomic i32   refs;
    bool          ok;
    FbSftpRow    *rows;
    i32           count;
} FbSftpListJob;


static void fb_sftp_rows_free(FbSftpRow *rows, i32 count) {
    for (i32 i = 0; i < count; i++) free(rows[i].name);
    free(rows);
}

static void fb_sftp_list_release(FbSftpListJob *j) {
    if (!j) return;
    if (atomic_fetch_sub_explicit(&j->refs, 1, memory_order_acq_rel) != 1) return;
    fb_sftp_rows_free(j->rows, j->count);
    if (j->session) session_release(j->session);
    free(j);
}

static void *fb_sftp_list_worker(void *arg) {
    FbSftpListJob *j = (FbSftpListJob *)arg;
#ifdef __APPLE__
    pthread_setname_np("liu-sftp-list");
#else
    pthread_setname_np(pthread_self(), "liu-sftp-list");
#endif
    i32 cap = 0;

    if (atomic_load_explicit(&j->cancel, memory_order_acquire)) goto publish;

    session_sftp_scope_begin(j->session);
    LIBSSH2_SFTP_HANDLE *dir;
    SFTP_CALL_PTR(dir, libssh2_sftp_opendir((LIBSSH2_SFTP *)j->sftp, j->path));
    if (dir) {
        char buf[512];
        LIBSSH2_SFTP_ATTRIBUTES attrs;
        j->ok = true;
        while (1) {
            if (atomic_load_explicit(&j->cancel, memory_order_acquire)) break;
            /* readdir does NOT NUL-terminate; a hostile/long remote filename
             * that fills the whole buffer leaves buf unterminated, so cap the
             * read at sizeof(buf)-1 and terminate before treating it as a C
             * string. */
            int rc;
            SFTP_CALL(rc, libssh2_sftp_readdir(dir, buf, sizeof(buf) - 1, &attrs));
            if (rc <= 0) break;
            if ((usize)rc >= sizeof(buf)) rc = sizeof(buf) - 1;
            buf[rc] = '\0';
            /* Only single, safe path components make it into the listing. The
             * server is the one choosing these strings, and every consumer
             * joins them onto a path — a name like "../../.ssh" would let a
             * hostile or compromised server steer a download outside the
             * folder the user is looking at. Also drops "." / ".." (the ".."
             * row is added on installation). */
            if (!sftp_name_is_safe_component(buf)) continue;
            if (j->count >= FB_MAX_ENTRIES - 1) break;   /* -1 leaves room for ".." */

            if (j->count >= cap) {
                i32 nc = cap ? cap * 2 : 64;
                if (nc > FB_MAX_ENTRIES) nc = FB_MAX_ENTRIES;
                FbSftpRow *grown = realloc(j->rows, (usize)nc * sizeof(*grown));
                if (!grown) break;
                j->rows = grown;
                cap = nc;
            }
            FbSftpRow *row = &j->rows[j->count];
            memset(row, 0, sizeof(*row));
            usize n = strlen(buf);
            row->name = malloc(n + 1);
            if (!row->name) break;
            memcpy(row->name, buf, n + 1);
            row->size        = attrs.filesize;
            row->permissions = attrs.permissions;
            row->mtime       = attrs.mtime;
            row->is_dir      = LIBSSH2_SFTP_S_ISDIR(attrs.permissions);
            /* readdir's attributes are the LINK's (the server lstat()s), so on
             * their own a link to a folder would be listed among the files and
             * opened as text. A link, and only a link, is asked what it points
             * at, as the local listing does; a dangling one stays a file.
             * is_symlink makes a delete take the link, not the folder. */
            row->is_symlink  = (attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) &&
                               LIBSSH2_SFTP_S_ISLNK(attrs.permissions);
            if (row->is_symlink) {
                char full[FB_MAX_PATH * 2];
                int fl = snprintf(full, sizeof full, "%s%s%s", j->path,
                                  !strcmp(j->path, "/") ? "" : "/", buf);
                LIBSSH2_SFTP_ATTRIBUTES target;
                memset(&target, 0, sizeof target);
                int src = -1;
                if (fl > 0 && (usize)fl < sizeof full)
                    SFTP_CALL(src, libssh2_sftp_stat((LIBSSH2_SFTP *)j->sftp, full, &target));
                if (src == 0 && (target.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS))
                    row->is_dir = LIBSSH2_SFTP_S_ISDIR(target.permissions);
            }
            j->count++;
        }
        { int crc; SFTP_CALL(crc, libssh2_sftp_closedir(dir)); (void)crc; }
    }
    session_sftp_scope_end(j->session);

publish:
    atomic_store_explicit(&j->done, true, memory_order_release);
    platform_wake_event_loop();
    fb_sftp_list_release(j);
    return NULL;
}

/* Give up on the listing in flight without waiting for it. */
static void fb_sftp_list_cancel(FileBrowser *fb) {
    if (!fb || !fb->sftp_list_job) return;
    FbSftpListJob *j = (FbSftpListJob *)fb->sftp_list_job;
    fb->sftp_list_job = NULL;
    atomic_store_explicit(&j->cancel, true, memory_order_release);
    fb_sftp_list_release(j);
}

/* Move a finished listing into the browser. Called from fb_poll_async on the
 * main thread, which is where every FileEntry has always been built. */
static void fb_sftp_list_install(FileBrowser *fb, FbSftpListJob *j) {
    if (j->generation != fb->nav_generation) return;   /* the user moved on */
    /* A listing that failed leaves the folder empty — the same thing the
     * sidebar shows for a local directory it cannot read. */
    if (j->tree) {
        FileEntry *rows = j->count ? calloc((usize)j->count, sizeof(FileEntry)) : NULL;
        bool ok = j->ok && (!j->count || rows);
        if (rows) {
            for (i32 i = 0; i < j->count; i++) {
                fb_entry_set_name(&rows[i], j->rows[i].name);
                if (!rows[i].name) ok = false;
                char full[FB_MAX_PATH];
                int n = snprintf(full, sizeof full, "%s%s%s", j->path,
                                 !strcmp(j->path, "/") ? "" : "/", j->rows[i].name);
                if (n >= 0 && (usize)n < sizeof full) rows[i].path = strdup(full);
                if (!rows[i].path) ok = false;
                rows[i].size = j->rows[i].size;
                rows[i].mtime = j->rows[i].mtime;
                rows[i].permissions = j->rows[i].permissions;
                rows[i].is_dir = j->rows[i].is_dir;
                rows[i].is_symlink = j->rows[i].is_symlink;
                rows[i].stat_known = true;
            }
            if (ok && j->count > 1) qsort(rows, (usize)j->count, sizeof(FileEntry), entry_cmp);
        }
        fb_tree_install(fb, j->path, rows, rows ? j->count : 0, ok);
        fb_local_rows_free(rows, rows ? j->count : 0);
        return;
    }
    if (!j->ok) { fb_entries_reset(fb); return; }

    fb_entries_reset(fb);
    i32 want = j->count + 1;                            /* +1 for ".." */
    if (want > FB_MAX_ENTRIES) want = FB_MAX_ENTRIES;
    if (fb->entry_cap < want) {
        FileEntry *grown = realloc(fb->entries, (usize)want * sizeof(FileEntry));
        if (!grown) return;
        fb->entries = grown;
        fb->entry_cap = want;
    }

    FileEntry *up = &fb->entries[fb->entry_count++];
    memset(up, 0, sizeof(*up));
    fb_entry_set_name(up, "..");
    up->is_dir = true;
    up->stat_known = true;

    for (i32 i = 0; i < j->count && fb->entry_count < fb->entry_cap; i++) {
        FileEntry *e = &fb->entries[fb->entry_count];
        memset(e, 0, sizeof(*e));
        fb_entry_set_name(e, j->rows[i].name);
        /* The name shown is NFC (fb_entry_set_name); the server's own bytes
         * name the file there. When the two differ the row keeps its real
         * path (fb_entry_path reads it, as the tree's rows do), so a download,
         * a delete or a rename acts on THIS file — not on a sibling spelt the
         * NFC way, and not on nothing (an NFD name a Mac wrote could not be
         * fetched at all). */
        if (e->name && strcmp(e->name, j->rows[i].name) != 0) {
            char full[FB_MAX_PATH];
            int n = snprintf(full, sizeof full, "%s%s%s", j->path,
                             !strcmp(j->path, "/") ? "" : "/", j->rows[i].name);
            if (n >= 0 && (usize)n < sizeof full) e->path = strdup(full);
            if (!e->path) { free(e->name); e->name = NULL; continue; }
        }
        e->size        = j->rows[i].size;
        e->permissions = j->rows[i].permissions;
        e->mtime       = j->rows[i].mtime;
        /* The remote listing already carried every attribute, so there is
         * nothing for fb_entry_ensure_stat to fetch — and it must not try, or
         * it would lstat a remote path against the LOCAL filesystem. */
        e->stat_known  = true;
        e->is_dir      = j->rows[i].is_dir;
        e->is_symlink  = j->rows[i].is_symlink;
        fb->entry_count++;
    }
    if (fb->entry_count > 1)
        qsort(fb->entries + 1, (usize)(fb->entry_count - 1), sizeof(FileEntry),
              entry_cmp);
    fb_tree_restore_apply(fb);
}

/* One child listing per browser; additional expanded rows queue in place.
 * The existing jobs retain their own storage/session and never touch the UI. */
static void fb_tree_start_pending(FileBrowser *fb) {
    if (!fb || fb->local_list_job || fb->sftp_list_job) return;
    for (i32 i = 0; i < fb->entry_count; i++) {
        FileEntry *e = &fb->entries[i];
        if (!e->expanded || !e->tree_pending) continue;
        char path[FB_MAX_PATH];
        fb_entry_path(fb, e, path, sizeof path);
        if (!path[0]) { e->expanded = e->tree_pending = false; continue; }
        if (fb->source == FB_SOURCE_LOCAL) {
            if (fb_local_list_live() >= FB_LOCAL_LIST_MAX_LIVE) return;
            FbLocalListJob *j = calloc(1, sizeof *j);
            if (!j) { e->expanded = e->tree_pending = false; continue; }
            snprintf(j->path, sizeof j->path, "%s", path);
            j->tree = true; j->generation = fb->nav_generation;
            j->delay_ms = fb_local_list_delay_ms();
            pthread_mutex_init(&j->mu, NULL); pthread_cond_init(&j->cv, NULL);
            atomic_init(&j->done, false); atomic_init(&j->cancel, false); atomic_init(&j->refs, 2);
            atomic_fetch_add_explicit(&g_fb_local_live, 1, memory_order_acq_rel);
            if (pthread_create(&j->thread, NULL, fb_local_list_worker, j) != 0) {
                atomic_fetch_sub_explicit(&g_fb_local_live, 1, memory_order_acq_rel);
                atomic_store(&j->refs, 1); fb_local_list_release(j);
                e->expanded = e->tree_pending = false;
                continue;
            }
            pthread_detach(j->thread);
            fb->local_list_job = j;
        } else {
            Session *owner = (Session *)fb->session;
            if (!fb->sftp_handle || !session_retain(owner)) {
                e->expanded = e->tree_pending = false; continue;
            }
            FbSftpListJob *j = calloc(1, sizeof *j);
            if (!j) { session_release(owner); e->expanded = e->tree_pending = false; continue; }
            snprintf(j->path, sizeof j->path, "%s", path);
            j->session = owner; j->sftp = fb->sftp_handle;
            j->tree = true; j->generation = fb->nav_generation;
            atomic_init(&j->done, false); atomic_init(&j->cancel, false); atomic_init(&j->refs, 2);
            if (pthread_create(&j->thread, NULL, fb_sftp_list_worker, j) != 0) {
                atomic_store(&j->refs, 1); fb_sftp_list_release(j);
                e->expanded = e->tree_pending = false; continue;
            }
            pthread_detach(j->thread);
            fb->sftp_list_job = j;
        }
        return;
    }
    fb_tree_restore_clear(fb); /* discard saved paths for folders that vanished */
}

static bool fb_tree_path_within(const char *path, const char *dir) {
    usize n = strlen(dir);
    return !strncmp(path, dir, n) && (path[n] == '\0' || path[n] == '/');
}

bool fb_toggle_directory(FileBrowser *fb, i32 index) {
    if (!fb || index < 0 || index >= fb->entry_count) return false;
    FileEntry *e = &fb->entries[index];
    if (!e->is_dir || !strcmp(e->name, "..")) return false;
    if (!e->expanded) {
        if (fb->entry_count >= FB_MAX_ENTRIES) return false;
        e->expanded = e->tree_pending = true;
        fb_tree_start_pending(fb);
        return true;
    }
    char path[FB_MAX_PATH];
    fb_entry_path(fb, e, path, sizeof path);
    FbLocalListJob *lj = fb->local_list_job;
    FbSftpListJob *sj = fb->sftp_list_job;
    if (lj && lj->tree && fb_tree_path_within(lj->path, path)) fb_local_list_cancel(fb);
    if (sj && sj->tree && fb_tree_path_within(sj->path, path)) fb_sftp_list_cancel(fb);
    if (fb->tree_restore) {
        for (i32 i = 0; i < fb->tree_restore->count; i++) {
            char *saved = fb->tree_restore->paths[i];
            if (saved && fb_tree_path_within(saved, path)) {
                free(saved); fb->tree_restore->paths[i] = NULL;
            }
        }
    }
    i32 end = index + 1;
    while (end < fb->entry_count && fb->entries[end].depth > e->depth) end++;
    for (i32 i = index + 1; i < end; i++) {
        free(fb->entries[i].name); free(fb->entries[i].path);
    }
    e->expanded = e->tree_pending = false;
    i32 removed = end - index - 1;
    memmove(fb->entries + index + 1, fb->entries + end,
            (usize)(fb->entry_count - end) * sizeof(FileEntry));
    fb->entry_count -= removed;
    fb_tree_rows_changed(fb, index + 1, removed, 0);
    fb_tree_start_pending(fb);
    return true;
}

bool fb_sftp_listing_active(const FileBrowser *fb) {
    return fb && fb->sftp_list_job != NULL;
}

void fb_poll_async(FileBrowser *fb) {
    if (!fb) return;
    fb_image_decode_reap(fb);
    fb_md_parse_reap(fb);
    if (fb->list_paused) return;
    FbLocalListJob *lj = fb->local_list_job;
    if (lj && atomic_load_explicit(&lj->done, memory_order_acquire)) {
        fb->local_list_job = NULL;
        if (lj->generation == fb->nav_generation) {
            /* A late failure leaves the folder empty, like a failed remote
             * listing; the browser already moved there. */
            fb_local_list_install(fb, lj);
            if (!lj->tree) fb_local_list_open_pending(fb);
        }
        fb->pending_open_name[0] = '\0';
        fb_local_list_release(lj);
    }
    FbSftpListJob *j = (FbSftpListJob *)fb->sftp_list_job;
    if (j && atomic_load_explicit(&j->done, memory_order_acquire)) {
        fb->sftp_list_job = NULL;
        fb_sftp_list_install(fb, j);
        fb_sftp_list_release(j);
    }
    fb_tree_start_pending(fb);
    fb_reveal_apply(fb);
}

bool fb_async_pending(const FileBrowser *fb) {
    if (!fb) return false;
    if (fb_image_decode_ready(fb)) return true;
    if (fb_md_parse_ready(fb)) return true;
    if (fb->list_paused) return false;
    const FbLocalListJob *lj = fb->local_list_job;
    if (lj && atomic_load_explicit(&lj->done, memory_order_acquire)) return true;
    const FbSftpListJob *j = (const FbSftpListJob *)fb->sftp_list_job;
    if (j && atomic_load_explicit(&j->done, memory_order_acquire)) return true;
    if (!lj && !j && (fb->source != FB_SOURCE_LOCAL || fb_local_list_live() < FB_LOCAL_LIST_MAX_LIVE))
        for (i32 i = 0; i < fb->entry_count; i++)
            if (fb->entries[i].expanded && fb->entries[i].tree_pending) return true;
    return false;
}

bool fb_navigate_sftp(FileBrowser *fb, void *sftp_handle, const char *path) {
    if (!fb || !sftp_handle || !path || !path[0]) return false;
    Session *owner = (Session *)fb->session;
    if (!owner) return false;

    if (fb->source == FB_SOURCE_SFTP && !strcmp(fb->cwd, path)) fb_tree_restore_capture(fb);
    else fb_tree_restore_clear(fb);
    /* Whatever was in flight is for a folder the user has left. */
    fb_sftp_list_cancel(fb);
    fb_local_list_cancel(fb);

    FbSftpListJob *j = calloc(1, sizeof *j);
    if (!j) return false;
    snprintf(j->path, sizeof j->path, "%s", path);
    if (!session_retain(owner)) { free(j); return false; }
    j->session = owner;
    j->sftp    = sftp_handle;
    atomic_init(&j->done, false);
    atomic_init(&j->cancel, false);
    atomic_init(&j->refs, 2);          /* this browser, and the worker */

    /* Commit the browser to the new folder before the listing exists: the
     * path bar and every consumer of fb->cwd must agree with what is being
     * fetched, and a result that arrives after the next navigation is
     * rejected by this generation. */
    fb->source = FB_SOURCE_SFTP;
    fb->sftp_handle = sftp_handle;
    char nav_cwd[sizeof fb->cwd];
    if (!platform_utf8_normalize_nfc(path, nav_cwd, sizeof nav_cwd))
        snprintf(nav_cwd, sizeof nav_cwd, "%s", path);
    snprintf(fb->cwd, sizeof(fb->cwd), "%s", nav_cwd);
    fb_entries_reset(fb);
    fb->md_only_entries = false;
    fb->selected = -1;
    fb_sel_clear(fb);
    fb->scroll_offset = 0;
    fb->scroll_offset_px = 0.0f;
    fb->nav_generation = ++g_fb_nav_generation;
    j->generation = fb->nav_generation;

    if (pthread_create(&j->thread, NULL, fb_sftp_list_worker, j) != 0) {
        session_release(owner);
        free(j);
        return false;
    }
    pthread_detach(j->thread);          /* nobody ever joins it */
    fb->sftp_list_job = j;
    return true;
}

void fb_detach_sftp(FileBrowser *fb) {
    if (!fb) return;
    fb_sftp_list_cancel(fb);
    fb_local_list_cancel(fb);
    fb->sftp_handle = NULL;
    fb->session = NULL;
    fb_tree_restore_clear(fb);
    fb_entries_reset(fb);
    fb->selected = -1;
    fb->scroll_offset = 0;
    fb->scroll_offset_px = 0.0f;
    fb->source = FB_SOURCE_LOCAL;
    fb->cwd[0] = '\0';
}

void fb_go_up(FileBrowser *fb) {
    char parent[FB_MAX_PATH];
    snprintf(parent, sizeof(parent), "%s", fb->cwd);
    char *last = strrchr(parent, '/');
    if (last && last != parent) *last = '\0';
    else snprintf(parent, sizeof(parent), "/");

    if (fb->source == FB_SOURCE_SFTP)
        fb_navigate_sftp(fb, fb->sftp_handle, parent);
    else
        fb_navigate(fb, parent);
}

void fb_refresh(FileBrowser *fb) {
    fb->reveal_armed = false;
    if (fb->source == FB_SOURCE_SFTP)
        fb_navigate_sftp(fb, fb->sftp_handle, fb->cwd);
    else
        fb_navigate(fb, fb->cwd);
}

/* Bring row `i` inside the list's viewport, moving the scroll as little as
 * that takes. Uses the metrics of the last draw; before any draw there is
 * nothing to scroll against and the renderer's own clamp is enough. */
static void fb_scroll_row_into_view(FileBrowser *fb, i32 i) {
    f32 row_h = fb->list_row_h, view = fb->viewport_height;
    if (row_h <= 0.0f || view <= 0.0f) return;
    f32 top = (f32)(i - fb_list_first(fb)) * row_h, bot = top + row_h;
    if (top < fb->scroll_offset_px) fb->scroll_offset_px = top;
    else if (bot > fb->scroll_offset_px + view) fb->scroll_offset_px = bot - view;
    if (fb->scroll_offset_px < 0.0f) fb->scroll_offset_px = 0.0f;
}

/* The second half of fb_refresh_keep, run every time rows land: put the
 * scroll back (once, with the root listing — a later call must not fight a
 * wheel), and select the wanted path as soon as a row carries it. Gives up
 * when nothing is left to arrive. */
static void fb_reveal_apply(FileBrowser *fb) {
    if (!fb->reveal_armed) return;
    /* The browser went somewhere else meanwhile: that folder's scroll and
     * selection are its own. */
    if (fb->reveal_nav_generation != (u64)fb->nav_generation) { fb->reveal_armed = false; return; }
    const FbLocalListJob *lj = fb->local_list_job;
    const FbSftpListJob *sj = (const FbSftpListJob *)fb->sftp_list_job;
    /* The one job slot of each kind carries the root listing or a folder's
     * children; only the root's is "the list is not here yet". */
    bool root_pending = (lj && !lj->tree) || (sj && !sj->tree);
    if (root_pending) return;
    if (fb->reveal_scroll_pending) {
        fb->scroll_offset_px = fb->reveal_scroll_px;    /* the draw clamps it */
        fb->reveal_scroll_pending = false;
    }
    bool more = lj || sj;
    for (i32 i = 0; i < fb->entry_count; i++) {
        const FileEntry *e = &fb->entries[i];
        if (e->expanded && e->tree_pending) more = true;
        if (!fb->reveal_path[0]) continue;
        char path[FB_MAX_PATH];
        fb_entry_path(fb, e, path, sizeof path);
        if (strcmp(path, fb->reveal_path) != 0) continue;
        fb->selected = i;
        fb_sel_clear(fb);
        fb_sel_set(fb, i, true);
        fb->selection_anchor = i;
        fb_scroll_row_into_view(fb, i);
        fb->reveal_armed = false;
        return;
    }
    if (!more || !fb->reveal_path[0]) fb->reveal_armed = false;
}

void fb_refresh_keep(FileBrowser *fb, const char *select_path) {
    if (!fb) return;
    char keep[FB_MAX_PATH] = "";
    if (select_path) snprintf(keep, sizeof keep, "%s", select_path);
    else if (fb->selected >= 0 && fb->selected < fb->entry_count)
        fb_entry_path(fb, &fb->entries[fb->selected], keep, sizeof keep);
    f32 scroll = fb->scroll_offset_px;
    fb_refresh(fb);
    snprintf(fb->reveal_path, sizeof fb->reveal_path, "%s", keep);
    /* Now, for the rows a slow refresh keeps on screen meanwhile, and again
     * when the listing lands (a remote one empties the list first, and the
     * draw clamps the offset to nothing). */
    fb->scroll_offset_px = scroll;
    fb->reveal_scroll_px = scroll;
    fb->reveal_scroll_pending = true;
    fb->reveal_nav_generation = (u64)fb->nav_generation;
    fb->reveal_armed = true;
    /* A fast local listing lands inside fb_navigate's own wait. */
    fb_reveal_apply(fb);
}

/* =========================================================================
 * File viewer — load + markdown bring-up
 *
 * Markdown is parsed into an MdDoc the moment a .md file opens; the AST
 * lives in fb->md_arena and is freed wholesale on close. Inline images
 * are loaded lazily through fb->md_images and stay resident until close.
 * The parser and its renderer live in src/ui/markdown.
 * ========================================================================= */

/* Syntax highlighting comes from ui/syntax.c — the one scanner the editor, the
 * diff dock and Markdown fenced code all share.
 *
 * Checkpoints keep backwards scrolling from rescanning the whole prefix. Edits
 * invalidate only the affected suffix; replacement drops all owners. */
#define FB_SYN_MAX_CHECKPOINTS 4096
#define FB_SYN_MIN_STRIDE 256
static void fb_syn_invalidate(FileBrowser *fb) {
    if (!fb) return;
    fb->syn_memo_valid = false;
    fb->syn_memo_line  = 0;
    fb->syn_memo_off   = 0;
    fb->syn_memo_state = 0;
    free(fb->syn_checkpoints);
    fb->syn_checkpoints = NULL;
    fb->syn_checkpoint_count = fb->syn_checkpoint_cap = fb->syn_checkpoint_stride = 0;
}

static void fb_syn_edited(FileBrowser *fb, usize offset) {
    if (fb->syn_memo_valid && fb->syn_memo_off > offset) fb->syn_memo_valid = false;
    while (fb->syn_checkpoint_count > 0 &&
           fb->syn_checkpoints[fb->syn_checkpoint_count-1].offset > offset)
        fb->syn_checkpoint_count--;
    if (!fb->syn_checkpoint_count) {
        free(fb->syn_checkpoints); fb->syn_checkpoints = NULL; fb->syn_checkpoint_cap = 0;
    } else if (fb->syn_checkpoint_cap >= 64 &&
               fb->syn_checkpoint_count <= fb->syn_checkpoint_cap / 4) {
        i32 cap = 32; while (cap < fb->syn_checkpoint_count) cap *= 2;
        FbSynCheckpoint *points = realloc(fb->syn_checkpoints, (usize)cap * sizeof(*points));
        if (points) { fb->syn_checkpoints = points; fb->syn_checkpoint_cap = cap; }
    }
}

static void fb_syn_checkpoint(FileBrowser *fb, i32 line, usize off, u32 state) {
    i32 n = fb->syn_checkpoint_count;
    if (line % fb->syn_checkpoint_stride || n >= FB_SYN_MAX_CHECKPOINTS ||
        (n && fb->syn_checkpoints[n-1].line >= line)) return;
    if (n == fb->syn_checkpoint_cap) {
        i32 cap = fb->syn_checkpoint_cap ? fb->syn_checkpoint_cap * 2 : 32;
        if (cap > FB_SYN_MAX_CHECKPOINTS) cap = FB_SYN_MAX_CHECKPOINTS;
        FbSynCheckpoint *points = realloc(fb->syn_checkpoints, (usize)cap * sizeof(*points));
        if (!points) return; /* the memo/full scan remains correct on OOM */
        fb->syn_checkpoints = points; fb->syn_checkpoint_cap = cap;
    }
    fb->syn_checkpoints[n] = (FbSynCheckpoint){off, state, line};
    fb->syn_checkpoint_count++;
}

/* Continuation and byte offset at a line start. A repeated viewport does no
 * prefix work; a backwards step starts at the nearest sparse checkpoint. */
static u32 fb_syn_state_at(FileBrowser *fb, i32 target_line) {
    if (!fb || !fb->view_content || fb->code_lang == SYN_LANG_NONE) return 0;
    if (target_line < 0) target_line = 0;
    i32 stride = fb->view_line_count > 0
        ? 1 + (fb->view_line_count - 1) / FB_SYN_MAX_CHECKPOINTS : FB_SYN_MIN_STRIDE;
    if (stride < FB_SYN_MIN_STRIDE) stride = FB_SYN_MIN_STRIDE;
    if (stride > fb->syn_checkpoint_stride) {
        fb->syn_checkpoint_stride = stride;
        fb->syn_checkpoint_count = 0;
        fb->syn_memo_valid = false;
    }

    i32   line = 0;
    u32   st   = 0;
    usize off  = 0;
    i32 lo = 0, hi = fb->syn_checkpoint_count;
    while (lo < hi) {
        i32 mid = lo + (hi-lo)/2;
        if (fb->syn_checkpoints[mid].line <= target_line) lo = mid+1; else hi = mid;
    }
    if (lo && fb->syn_checkpoints[lo-1].offset <= fb->view_size) {
        const FbSynCheckpoint *p = &fb->syn_checkpoints[lo-1];
        line = p->line; off = p->offset; st = p->state;
    }
    if (fb->syn_memo_valid && fb->syn_memo_line <= target_line &&
        fb->syn_memo_line >= line &&
        fb->syn_memo_off <= fb->view_size) {
        line = fb->syn_memo_line;
        st   = fb->syn_memo_state;
        off  = fb->syn_memo_off;
    }

    while (line < target_line && off < fb->view_size) {
        /* By length: a NUL in the file is a byte of the line, not its end. */
        usize brk = editor_line_break(fb->view_content, fb->view_size, off);
        usize end = editor_line_text_end(fb->view_content, fb->view_size, off, brk);
        syntax_advance_state(fb->code_lang, fb->view_content + off, (u32)(end - off), &st);
        off = brk < fb->view_size ? brk + 1 : brk;
        line++;
        fb_syn_checkpoint(fb, line, off, st);
    }

    fb->syn_memo_valid = true;
    fb->syn_memo_line  = line;
    fb->syn_memo_off   = off;
    fb->syn_memo_state = st;
    return st;
}

static char *fb_code_view_start(FileBrowser *fb, i32 target, u32 *state, i32 *line_num) {
    *state = 0; *line_num = 0;
    if (!fb || !fb->view_content) return NULL;
    if (fb->code_lang == SYN_LANG_NONE) {
        char *line = editor_line_start(fb, target);
        *line_num = fb->editor_line_cache_line;
        return line;
    }
    *state = fb_syn_state_at(fb, target);
    *line_num = fb->syn_memo_line;
    /* Edits/caret actions can reuse the same line position too. */
    fb->editor_line_cache_buffer = fb->view_content;
    fb->editor_line_cache_offset = fb->syn_memo_off;
    fb->editor_line_cache_line = fb->syn_memo_line;
    fb->editor_line_cache_valid = true;
    return fb->view_content + fb->syn_memo_off;
}

/* The bytes of a line that fall in the columns [first, first + columns): where
 * the painter starts, where it may stop, and the column its first cell begins
 * at — which is LEFT of `first` when a tab straddles the edge. Walked with
 * editor_cell_at, the painter's own step, so a tab, a byte that is not UTF-8
 * and a NUL take here exactly the columns they are drawn in. */
typedef struct { i32 begin, end, column; } FbCodeSlice;
static FbCodeSlice fb_code_visible_slice(const char *line, i32 len, i32 first, i32 columns) {
    if (first < 0) first = 0;
    if (columns < 0) columns = 0;
    i32 pos = 0, col = 0;
    while (pos < len) {
        EditorCell c = editor_cell_at(line, len, pos, col, FB_TAB_COLS);
        if (col + c.cols > first) break;
        pos += c.bytes;
        col += c.cols;
    }
    FbCodeSlice slice = {.begin=pos, .column=col};
    while (pos < len && col < first + columns) {
        EditorCell c = editor_cell_at(line, len, pos, col, FB_TAB_COLS);
        pos += c.bytes;
        col += c.cols;
    }
    slice.end = pos;
    return slice;
}

/* Drop whatever the image viewer was holding. Every path that switches the viewer
 * to a *non*-image document has to call this, not just the explicit close.
 *
 * A stale `view_image_is_animated` keeps main.c's GIF pacing block choosing this
 * browser as the animation driver while fb_render_viewer, no longer in
 * FVIEW_IMAGE, stops refreshing `view_image_next_frame_at`: the deadline stays in
 * the past, the loop redraws at full rate, and the GIF's RGBA buffer (up to 128
 * MiB) stays pinned. */
static void fb_image_state_reset(FileBrowser *fb) {
    if (!fb) return;
    usize one = 0, all = 0;
    if (fb->view_image_rgba &&
        img_rgba_bytes_checked(fb->view_image_w, fb->view_image_h, &one) &&
        checked_mul_usize(one, (usize)(fb->view_image_frame_count > 0
                                      ? fb->view_image_frame_count : 1), &all))
        mem_budget_adjust(MEM_IMAGE_CPU, -(isize)all);
    if (fb->view_image_rgba) {
        stbi_image_free(fb->view_image_rgba);
        fb->view_image_rgba = NULL;
    }
    free(fb->view_image_frame_delays);
    fb->view_image_frame_delays  = NULL;
    fb->view_image_frame_count   = 0;
    fb->view_image_frame_index   = 0;
    fb->view_image_frame_anchor  = 0.0;
    fb->view_image_next_frame_at = 0.0;
    fb->view_image_is_animated   = false;
    fb->view_image_svg           = false;
    fb->view_image_ink_dark      = false;
    fb->view_image_w = 0;
    fb->view_image_h = 0;
    fb->view_image_zoom  = 0.0f;
    fb->view_image_pan_x = 0.0f;
    fb->view_image_pan_y = 0.0f;
}

/* Release the per-render markdown scratch buffers (glyph/link/task/fold rects
 * plus the outline) and zero their counts.
 *
 * They are lazily reallocated by the next Markdown render, so every path that
 * leaves the Markdown viewer — closing it *or* opening a non-Markdown file —
 * can drop them; leaving them pinned turns any tab that ever showed one note
 * into a permanent ~241 KiB hold on the heap. The counts are cleared with the
 * buffers because the rect/outline entries index slices that live in the md
 * arena, which those same paths destroy. */
static void md_scratch_release(FileBrowser *fb) {
    if (!fb) return;
    free(fb->md_glyph_rects);
    fb->md_glyph_rects = NULL;
    fb->md_glyph_count = 0;
    free(fb->md_link_rects);
    fb->md_link_rects = NULL;
    fb->md_link_count = 0;
    free(fb->md_task_rects);
    fb->md_task_rects = NULL;
    fb->md_task_count = 0;
    free(fb->md_fold_rects);
    fb->md_fold_rects = NULL;
    fb->md_fold_count = 0;
    free(fb->md_outline);
    fb->md_outline = NULL;
    fb->md_outline_count = 0;
}

/* Commit a prepared text buffer. Markdown opens prepare their source, line
 * count, AST and caches on a worker before this commit point. */
static void fb_text_install(FileBrowser *fb, const char *path, FileViewMode mode,
                            char *content, usize len, usize cap, i32 lines) {
    /* Past every early return, so the open is committed to this file:
     * release the previously-viewed image before the viewer switches to
     * text/markdown. Placed here rather than at the top of the function so
     * a failed open leaves the currently-displayed image intact. */
    fb_image_decode_cancel(fb);
    fb_image_state_reset(fb);

    /* Same commit point, same reasoning, for the markdown side. This used
     * to sit inside the `mode == FVIEW_MARKDOWN` branch below, so opening a
     * .c/.txt/.png right after a note left the previous document's arena
     * (sized up to MB(192)), its decoded-image cache and its embed cache
     * alive for as long as the browser existed. The teardown is idempotent
     * and nothing between here and the parse below reads md state, so the
     * markdown branch simply builds on a clean slate. */
    if (fb->md_images) { md_image_cache_destroy(fb->md_images); fb->md_images = NULL; }
    if (fb->md_embeds) { md_embed_cache_destroy(fb->md_embeds); fb->md_embeds = NULL; }
    if (fb->md_arena.base) { arena_destroy(&fb->md_arena); }
    fb_md_parse_cancel(fb); free(fb->md_src); fb->md_src = NULL;
    fb->md_doc = NULL;
    fb->md_link_count = fb->md_glyph_count = fb->md_outline_count = 0;
    fb->md_task_count = fb->md_fold_count = 0;
    fb->md_copy_pending = false;
    fb->md_seek_block = MD_SEEK_NONE;
    fb->md_seek_y = 0.0f;
    /* Leaving markdown altogether: the render scratch is dead weight until
     * some other note is opened, and its rects index the arena just freed.
     * Kept when the new file is itself markdown — the next render refills
     * the same buffers, so freeing them would only churn the allocator. */
    if (mode != FVIEW_MARKDOWN) md_scratch_release(fb);

    editor_line_cache_drop(fb);
    free(fb->view_content);
    fb->view_content = content;
    fb->view_size = len;
    fb->view_cap = cap;
    /* Another text: whatever was worked out from the last one is stale (the
     * Find matches, the encoding label), the ending Enter writes is this
     * file's, and nothing is known yet about where it sits on disk — the
     * read that produced it says so right after (fb_md_parse_reap). */
    fb->view_edit_gen++;
    fb->view_crlf = editor_detect_crlf(content, len);
    fb->view_disk = (FileIdentity){0};
    unsaved_guard_disarm(&fb->guard);

    snprintf(fb->view_path, sizeof(fb->view_path), "%s", path);
    fb->view_mode = mode;
    fb->view_scroll = 0;
    fb->view_scroll_px = 0.0f;
    fb->view_content_px = 0.0f;
    fb->md_table_scroll_x = fb->md_table_overflow = 0.0f;
    fb->md_restore_read_scroll = false;
    fb->live_layout_cache.valid = false;
    fb->view_scroll_x = 0;
    fb->hscroll_accum_px = 0.0f;
    fb->vscroll_accum_px = 0.0f;
    fb->editor_mode = (fb->view_mode == FVIEW_CODE || fb->view_mode == FVIEW_TEXT);
    fb->cursor_line = 0;
    fb->cursor_col = 0;
    fb->modified = false;
    /* Drop any markdown selection from the previously-viewed file. The
     * glyph rects backing it belong to a doc about to be freed (or to a
     * doc that simply no longer matches the new view). Without this,
     * fb_md_selection_active() would keep returning true after
     * navigating to a text/code/image file, and the Cmd+C path would
     * copy stale text from the old Markdown render. */
    fb->md_sel_active = false;

    /* Highlighter language for the new buffer, resolved once from the path
     * (the scanner re-derives nothing per line). Cleared memo: the previous
     * file's continuation state says nothing about this one. */
    fb->code_lang = syntax_lang_for_path(fb->view_path, strlen(fb->view_path));
    fb_syn_invalidate(fb);

    fb->view_line_count = lines;

    if (fb->view_mode == FVIEW_MARKDOWN) {
        /* Every markdown open lands in Read mode — otherwise a note opened
         * while the previous one was in Edit inherits md_raw_mode=true with
         * editor_mode=false (raw shown but not editable, Cmd+E stuck). */
        fb->md_raw_mode = false;
        fb->md_source_dirty = false;
        /* Open with a narrow list sidebar so the renderer fills the window
         * (consumed by render_fb_tab; resets even if a previous file left a
         * wide dragged divider). */
        fb->reset_split_narrow = true;
        /* Any previous markdown state (arena + image/embed caches) was
         * already torn down at the commit point above, unconditionally, so
         * that non-markdown opens release it too; md_sel_active was cleared
         * in the general-reset block. Nothing MD-specific to free here. */

    }

    fb_editor_reset_undo(fb);
}

bool fb_open_file(FileBrowser *fb, i32 index) {
    if (index < 0 || index >= fb->entry_count) return false;
    FileEntry *e = &fb->entries[index];
    char entry_path[FB_MAX_PATH * 2];
    fb_entry_path(fb, e, entry_path, sizeof(entry_path));

    if (e->is_dir) {
        if (strcmp(e->name, "..") == 0) {
            fb_go_up(fb);
        } else if (fb->view_mode != FVIEW_GRAPH) {
            return fb_toggle_directory(fb, index);
        } else {
            if (fb->source == FB_SOURCE_SFTP)
                fb_navigate_sftp(fb, fb->sftp_handle, entry_path);
            else
                fb_navigate(fb, entry_path);
        }
        return true;
    }

    if (fb->source != FB_SOURCE_LOCAL) return false;
    /* Opening a document is asking to work in it: it takes the keyboard (the
     * decode / read below lands on a later frame, so the grant is here). */
    fb->kbd_away = false;
    FileViewMode mode = fb_detect_mode(e->name);
    if (mode == FVIEW_IMAGE && fb->open_svg_source && fb_path_is_svg(e->name))
        mode = FVIEW_CODE;               /* an open that asked to edit wants the markup */
    char fallback[FB_MAX_PATH * 2];
    snprintf(fallback, sizeof fallback, "%s/%s", fb->cwd, e->name);
    /* The document that is already open, with edits in it: a click on its
     * own row is not a request to read the file again over them. */
    if (fb_editor_dirty(fb) &&
        (!strcmp(fb->view_path, entry_path) || !strcmp(fb->view_path, fallback)))
        return true;
    if (mode == FVIEW_IMAGE) return fb_image_decode_start(fb, entry_path);
    return fb_text_open_start(fb, entry_path, fallback, mode);
}

/* Where a note may transclude from (see md_embed.h): the notes Vault, the
 * folder the explorer is showing, and the document's own folder. */
static void fb_md_embed_roots(FileBrowser *fb) {
    if (!fb) return;
    if (fb->md_images) fb->md_images->async_headers = true;
    if (!fb->md_embeds) return;
    md_embed_cache_enable_async(fb->md_embeds, platform_wake_event_loop);
    char doc_dir[FB_MAX_PATH];
    snprintf(doc_dir, sizeof doc_dir, "%s", fb->view_path);
    char *slash = strrchr(doc_dir, '/');
    if (slash && slash != doc_dir) *slash = '\0';
    else if (slash == doc_dir) doc_dir[1] = '\0';
    else doc_dir[0] = '\0';
    const char *roots[3] = { getenv("LIU_VAULT"), fb->cwd, doc_dir };
    md_embed_cache_set_roots(fb->md_embeds, roots, 3);
}

/* ---- Markdown parse, synchronous or off-thread -------------------------
 *
 * File opens read and parse entirely on a worker. Edited-buffer reparses
 * below FB_MD_ASYNC_MIN stay synchronous, borrowing view_content. Above
 * that small bound the parse is a job: the
 * worker parses its OWN copy of the source into its own arena (the AST
 * borrows its source, and the editor may realloc view_content meanwhile),
 * and fb_poll_async adopts arena + copy + doc in one step. Until then md_doc
 * is NULL and the viewer shows the raw text, which is what it already does
 * for a doc it has no AST for. The copy costs one document's bytes for the
 * life of the AST, next to an arena budgeted at ~8x that. */

#define FB_MD_ASYNC_MIN KB(8)
#define FB_MD_MAX_LIVE 8
static usize g_fb_md_async_min = FB_MD_ASYNC_MIN;
void fb_md_set_async_min(usize bytes) { g_fb_md_async_min = bytes; }
static i32 g_fb_md_read_delay_ms;
void fb_md_set_read_delay(i32 ms) { g_fb_md_read_delay_ms = ms > 0 ? ms : 0; }
static _Atomic i32 g_fb_md_live;

static bool fb_md_admit(void) {
    i32 live = atomic_load_explicit(&g_fb_md_live, memory_order_relaxed);
    while (live < FB_MD_MAX_LIVE) {
        if (atomic_compare_exchange_weak_explicit(&g_fb_md_live, &live, live + 1,
                                                  memory_order_acq_rel, memory_order_relaxed))
            return true;
    }
    return false;
}

typedef struct FbMdParseJob {
    Arena         arena;
    u8           *src;
    usize         len;
    char         *content;          /* opening: editable source, read by worker */
    usize         cap;
    i32           lines;
    FileIdentity  disk;             /* opening: the file `content` was read from */
    char          path[FB_MAX_PATH];
    char          fallback[FB_MAX_PATH];
    bool          opening;
    FileViewMode  mode;
    bool          edit_on_open;
    bool          loaded;
    i32           read_delay_ms;
    char          seek_anchor[256]; /* main-thread-only deferred section jump */
    bool          seek_is_block;
    char          vault_root[FB_MAX_PATH];
    char          cwd_root[FB_MAX_PATH];
    MdImageCache *images;
    MdEmbedCache *embeds;
    char          base_dir[FB_MAX_PATH];
    MdDoc        *doc;
    _Atomic bool  done;
    _Atomic bool  cancel;
    _Atomic i32   refs;
} FbMdParseJob;

static usize fb_md_arena_est(usize len) {
    usize est = len * 8 + (len / 2) * (sizeof(MdInline) + sizeof(MdBlock));
    if (est < KB(64))  est = KB(64);
    if (est > MB(192)) est = MB(192);
    return est;
}

static void fb_md_parse_release(FbMdParseJob *j) {
    if (!j) return;
    if (atomic_fetch_sub_explicit(&j->refs, 1, memory_order_acq_rel) != 1) return;
    if (j->arena.base) arena_destroy(&j->arena);
    free(j->src);
    free(j->content);
    if (j->images) md_image_cache_destroy(j->images);
    if (j->embeds) md_embed_cache_destroy(j->embeds);
    free(j);
}

/* The largest file the viewer reads. */
#define FB_OPEN_MAX_BYTES (2 * 1024 * 1024)

/* Read a whole file for the viewer: its bytes plus the terminator the editor
 * keeps after them, its line count, and WHICH file it was.
 *
 * The identity comes from the descriptor, before the first byte is read. A
 * writer that gets in during the read moves the file's mtime past what is
 * recorded here, so the view learns its copy is not the file; taking it after
 * the read would record the newer file against the older bytes.
 *
 * False when the file cannot be opened, is not a regular file, is larger than
 * the viewer takes, or the read failed; `*content` is NULL then. Runs on the
 * open worker — and on the main thread only where the same file is about to
 * be written from it anyway (fb_view_reload_now). */
/* Why a document did not open, on the browser's status line. */
static void fb_notice_open_failed(FileBrowser *fb, const char *path) {
    const char *slash = strrchr(path, '/');
    const char *name = slash ? slash + 1 : path;
    struct stat st;
    if (stat(path, &st) != 0)
        fb_notice(fb, true, "Could not open %s: %s", name, strerror(errno));
    else if (S_ISREG(st.st_mode) && st.st_size > FB_OPEN_MAX_BYTES)
        fb_notice(fb, true, "%s is too large to open here (over %d MB)", name,
                  (int)(FB_OPEN_MAX_BYTES / (1024 * 1024)));
    else if (!S_ISREG(st.st_mode))
        fb_notice(fb, true, "%s is not a file that can be opened", name);
    else if (access(path, R_OK) != 0)
        fb_notice(fb, true, "Could not open %s: %s", name, strerror(errno));
    else
        fb_notice(fb, true, "Could not read %s", name);
}

static bool fb_file_read(const char *path, char **content, usize *len, usize *cap,
                         i32 *lines, FileIdentity *disk) {
    *content = NULL; *len = 0; *cap = 0; *lines = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    struct stat st;
    bool ok = fstat(fileno(f), &st) == 0 && S_ISREG(st.st_mode) &&
              st.st_size >= 0 && st.st_size <= FB_OPEN_MAX_BYTES;
    char *buf = ok ? malloc((usize)st.st_size + 1) : NULL;
    if (!buf) { fclose(f); return false; }
    usize n = fread(buf, 1, (usize)st.st_size, f);
    ok = !ferror(f);
    fclose(f);
    if (!ok) { free(buf); return false; }
    buf[n] = '\0';
    *content = buf;
    *len = n;
    *cap = (usize)st.st_size + 1;
    *lines = 1;
    for (usize i = 0; i < n; i++)
        if (buf[i] == '\n') (*lines)++;
    if (disk) *disk = file_identity_of(&st);
    return true;
}

static void *fb_md_parse_worker(void *arg) {
    FbMdParseJob *j = (FbMdParseJob *)arg;
#ifdef __APPLE__
    pthread_setname_np("liu-md-parse");
#else
    pthread_setname_np(pthread_self(), "liu-md-parse");
#endif
    if (j->read_delay_ms > 0) {
        struct timespec ts = { j->read_delay_ms / 1000,
                               (long)(j->read_delay_ms % 1000) * 1000000L };
        nanosleep(&ts, NULL);
    }
    if (j->opening && !atomic_load_explicit(&j->cancel, memory_order_acquire)) {
        j->loaded = fb_file_read(j->path, &j->content, &j->len, &j->cap, &j->lines, &j->disk);
        if (!j->loaded && strcmp(j->path, j->fallback) &&
            fb_file_read(j->fallback, &j->content, &j->len, &j->cap, &j->lines, &j->disk)) {
            j->loaded = true;
            snprintf(j->path, sizeof j->path, "%s", j->fallback);
        }
        if (j->loaded && j->mode == FVIEW_MARKDOWN) {
            j->src = malloc(j->len ? j->len : 1);
            if (j->src) memcpy(j->src, j->content, j->len);
        }
        snprintf(j->base_dir, sizeof j->base_dir, "%s", j->path);
        char *slash = strrchr(j->base_dir, '/');
        if (slash == j->base_dir) slash[1] = '\0';
        else if (slash) *slash = '\0';
        else snprintf(j->base_dir, sizeof j->base_dir, ".");
    }
    if (j->src && !atomic_load_explicit(&j->cancel, memory_order_acquire)) {
        j->arena = arena_create(fb_md_arena_est(j->len));
        if (j->arena.base) {
            j->doc = md_parse_ex(&j->arena, j->src, j->len, NULL, &j->cancel);
            if (j->doc && j->doc->incomplete) j->doc = NULL;
            if (j->doc) {
                usize bn = strlen(j->base_dir) + 1;
                char *bd = arena_alloc(&j->arena, bn);
                if (bd) { memcpy(bd, j->base_dir, bn); j->doc->base_dir = bd; }
            }
        }
    }
    if (j->mode == FVIEW_MARKDOWN && (!j->opening || j->loaded) &&
        !atomic_load_explicit(&j->cancel, memory_order_acquire)) {
        j->images = md_image_cache_create();
        if (j->images) j->images->async_headers = true;
        j->embeds = md_embed_cache_create();
        if (j->embeds) {
            md_embed_cache_enable_async(j->embeds, platform_wake_event_loop);
            const char *roots[] = {j->vault_root, j->cwd_root, j->base_dir};
            /* set_roots resolves real paths, including network-mounted Vaults. */
            md_embed_cache_set_roots(j->embeds, roots, 3);
        }
    }
    atomic_store_explicit(&j->done, true, memory_order_release);
    atomic_fetch_sub_explicit(&g_fb_md_live, 1, memory_order_acq_rel);
    platform_wake_event_loop();
    fb_md_parse_release(j);
    return NULL;
}

static void fb_md_base_dir(const FileBrowser *fb, char *out, usize cap) {
    snprintf(out, cap, "%s", fb->view_path);
    char *slash = strrchr(out, '/');
    if (slash == out) slash[1] = '\0';
    else if (slash) *slash = '\0';
    else snprintf(out, cap, ".");
}

static void fb_md_parse_cancel(FileBrowser *fb) {
    if (!fb || !fb->md_parse_job) return;
    FbMdParseJob *j = fb->md_parse_job;
    fb->md_parse_job = NULL;
    atomic_store_explicit(&j->cancel, true, memory_order_release);
    fb_md_parse_release(j);
}

static bool fb_md_parse_ready(const FileBrowser *fb) {
    return fb && fb->md_parse_job &&
           atomic_load_explicit(&fb->md_parse_job->done, memory_order_acquire);
}

bool fb_md_parse_active(const FileBrowser *fb) {
    return fb && fb->md_parse_job && fb->md_parse_job->mode == FVIEW_MARKDOWN;
}

bool fb_file_open_active(const FileBrowser *fb) {
    return fb && fb->md_parse_job && fb->md_parse_job->opening;
}

static bool fb_md_open_active(const FileBrowser *fb) {
    return fb && fb->md_parse_job && fb->md_parse_job->opening;
}

static void fb_open_set_edit(FileBrowser *fb, bool edit) {
    if (!edit) return;
    if (fb_file_open_active(fb)) fb->md_parse_job->edit_on_open = true;
    else if (fb->view_mode == FVIEW_MARKDOWN && !fb->md_raw_mode) fb_md_toggle_edit(fb);
}

/* fb_open_file, then the edit the caller asked for. An SVG asked to be edited
 * opens as its markup (open_svg_source); every other file opens as
 * fb_open_file opens it. */
static bool fb_open_file_edit(FileBrowser *fb, i32 index, bool edit) {
    fb->open_svg_source = edit;
    bool ok = fb_open_file(fb, index);
    fb->open_svg_source = false;
    if (ok) fb_open_set_edit(fb, edit);
    return ok;
}

static void fb_md_job_roots(FbMdParseJob *j, const FileBrowser *fb) {
    const char *vault = getenv("LIU_VAULT");
    if (vault && strlen(vault) < sizeof j->vault_root)
        snprintf(j->vault_root, sizeof j->vault_root, "%s", vault);
    snprintf(j->cwd_root, sizeof j->cwd_root, "%s", fb->cwd);
}

/* Every disk open is a worker job, regardless of document size. A small file
 * can live on a stalled mount too; completion only transfers prepared owners. */
static bool fb_text_open_start(FileBrowser *fb, const char *path, const char *fallback, FileViewMode mode) {
    if (strlen(path) >= FB_MAX_PATH || strlen(fallback) >= FB_MAX_PATH) return false;
    /* The loading marker installed below replaces the open text at once, long
     * before the new file is known to be readable: this is where the edits in
     * it would go. */
    if (!fb_discard_ok(fb, UNSAVED_OPEN)) return false;
    FbMdParseJob *j = calloc(1, sizeof *j);
    char *marker = calloc(1, 1);
    if (!j || !marker) { free(j); free(marker); return false; }
    snprintf(j->path, sizeof j->path, "%s", path);
    snprintf(j->fallback, sizeof j->fallback, "%s", fallback);
    j->opening = true;
    j->mode = mode;
    fb_md_job_roots(j, fb);
    j->read_delay_ms = g_fb_md_read_delay_ms;
    atomic_init(&j->done, false);
    atomic_init(&j->cancel, false);
    atomic_init(&j->refs, 2);
    pthread_t tid;
    if (!fb_md_admit()) { free(j); free(marker); return false; }
    if (pthread_create(&tid, NULL, fb_md_parse_worker, j) != 0) {
        atomic_fetch_sub_explicit(&g_fb_md_live, 1, memory_order_acq_rel);
        free(j); free(marker); return false;
    }
    pthread_detach(tid);
    fb_text_install(fb, path, mode, marker, 0, 1, 0);
    fb->editor_mode = false; /* loading marker is never editable */
    fb->md_parse_job = j;
    return true;
}

/* Adopt a finished parse: arena, source copy and doc change owner. */
static void fb_md_parse_reap(FileBrowser *fb) {
    if (!fb_md_parse_ready(fb)) return;
    FbMdParseJob *j = fb->md_parse_job;
    fb->md_parse_job = NULL;
    if (j->opening) {
        if (fb->view_mode == j->mode && j->loaded) {
            fb_text_install(fb, j->path, j->mode, j->content, j->len, j->cap, j->lines);
            j->content = NULL;
            fb->view_disk = j->disk;     /* which file this text is a copy of */
        } else {
            /* Keep a failed open empty; it cannot become an editable new file.
             * And say why: the viewer went blank with nothing said (a file
             * over the size the viewer reads, one it may not read). */
            if (fb->view_mode == j->mode) fb_notice_open_failed(fb, j->path);
            fb->view_mode = FVIEW_NONE;
        }
    }
    if (fb->view_mode == FVIEW_MARKDOWN) {
        if (fb->md_images) md_image_cache_destroy(fb->md_images);
        if (fb->md_embeds) md_embed_cache_destroy(fb->md_embeds);
        fb->md_images = j->images;
        fb->md_embeds = j->embeds;
        j->images = NULL;
        j->embeds = NULL;
    }
    if (fb->view_mode == FVIEW_MARKDOWN && j->doc) {
        if (fb->md_arena.base) arena_destroy(&fb->md_arena);
        free(fb->md_src);
        fb->md_arena = j->arena;
        fb->md_src   = j->src;
        fb->md_doc   = j->doc;
        memset(&j->arena, 0, sizeof j->arena);
        j->src = NULL;
        j->doc = NULL;
        if (j->seek_anchor[0]) fb_md_seek_anchor(fb, j->seek_anchor, j->seek_is_block);
    }
    if (j->opening && j->loaded && j->edit_on_open && fb->view_mode == FVIEW_MARKDOWN)
        fb_md_toggle_edit(fb);
    fb_md_parse_release(j);
}

/* Rebuild an edited source. The caller cleared the old AST/collectors; tiny
 * reparses retain this document's caches, larger jobs prepare new owners. */
static void fb_md_build(FileBrowser *fb) {
    fb_md_parse_cancel(fb);
    free(fb->md_src);
    fb->md_src = NULL;
    fb->md_doc = NULL;
    usize len = fb->view_size;
    bool async_ok = len >= g_fb_md_async_min && fb->view_content;
    FbMdParseJob *j = NULL;
    if (async_ok) {
        j = calloc(1, sizeof *j);
        if (j) j->src = malloc(len ? len : 1);
        if (j && j->src) {
            memcpy(j->src, fb->view_content, len);
            j->len = len;
            j->mode = FVIEW_MARKDOWN;
            fb_md_job_roots(j, fb);
            fb_md_base_dir(fb, j->base_dir, sizeof j->base_dir);
            atomic_init(&j->done, false);
            atomic_init(&j->cancel, false);
            atomic_init(&j->refs, 2);
            pthread_t tid;
            bool admitted = fb_md_admit();
            if (admitted && pthread_create(&tid, NULL, fb_md_parse_worker, j) == 0) {
                pthread_detach(tid);
                fb->md_parse_job = j;
            } else {
                if (admitted) atomic_fetch_sub_explicit(&g_fb_md_live, 1, memory_order_acq_rel);
                free(j->src); free(j); j = NULL;
            }
        } else {
            if (j) free(j);
            j = NULL;
        }
    }
    if (!async_ok) {
        /* Only bounded edited documents parse here. Resource exhaustion leaves
         * a large document in the raw viewer rather than stalling a frame. */
        fb->md_arena = arena_create(fb_md_arena_est(len));
        fb->md_doc = md_parse(&fb->md_arena, (const u8 *)fb->view_content, len, NULL);
        if (fb->md_doc && fb->md_doc->incomplete) fb->md_doc = NULL;
        if (fb->md_doc) {
            char tmp[FB_MAX_PATH];
            fb_md_base_dir(fb, tmp, sizeof tmp);
            usize bn = strlen(tmp) + 1;
            char *bd = arena_alloc(&fb->md_arena, bn);
            if (bd) { memcpy(bd, tmp, bn); fb->md_doc->base_dir = bd; }
        }
    }
    if (!async_ok) {
        /* Tiny reparses retain caches for this same document. Re-resolving
         * identical embed roots would put filesystem I/O on the editor path. */
        if (!fb->md_images) fb->md_images = md_image_cache_create();
        if (!fb->md_embeds) {
            fb->md_embeds = md_embed_cache_create();
            fb_md_embed_roots(fb);
        }
    }
}

/* Re-parse the current (possibly edited) markdown buffer into a fresh MdDoc so
 * the rendered preview reflects edits made in write mode. Mirrors the parse the
 * open path does, but reads the live view_content/view_size. */
static void fb_md_reparse(FileBrowser *fb) {
    if (!fb || fb->view_mode != FVIEW_MARKDOWN || !fb->view_content) return;
    /* Entering and leaving Edit without a mutation keeps the AST, layout and
     * image/embed jobs. `modified` cannot serve this purpose: saving clears
     * it even when the preview still describes the previous source. */
    if ((fb->md_doc || fb->md_parse_job) && !fb->md_source_dirty) return;
    fb_md_parse_cancel(fb);
    if (fb->md_arena.base) { arena_destroy(&fb->md_arena); }
    fb_md_parse_cancel(fb); free(fb->md_src); fb->md_src = NULL;
    fb->md_doc = NULL;
    /* The link/glyph/outline collectors hold url/text slices that point into
     * the arena just freed. Zero their counts now so no consumer (link/copy
     * hit-test, outline picker) dereferences a dangling pointer before the
     * next render repopulates them — important if md_parse below fails and the
     * rendered branch (which resets them) is skipped every frame. */
    fb->md_link_count = 0;
    fb->md_glyph_count = 0;
    fb->md_outline_count = 0;
    fb->md_task_count = 0;
    fb->md_fold_count = 0;
    fb->md_source_dirty = false;
    fb_md_build(fb);
}

/* Toggle a markdown viewer between Read (rendered preview) and Edit (raw,
 * editable) modes — a two-pane preview/edit split. Entering Edit turns
 * on the text editor; leaving it re-parses the buffer so the preview is fresh. */
void fb_md_toggle_edit(FileBrowser *fb) {
    if (!fb || fb->view_mode != FVIEW_MARKDOWN || fb_md_open_active(fb)) return;
    fb->live_layout_cache.valid = false;
    if (fb->md_raw_mode) {
        /* Edit → Read */
        fb->md_raw_mode = false;
        fb->editor_mode = false;
        fb_md_reparse(fb);
        fb->view_scroll_px = fb->md_read_saved_scroll;
        fb->md_restore_read_scroll = true;
    } else {
        /* Read → Edit */
        fb->md_read_saved_scroll = fb->view_scroll_px;
        fb->md_raw_mode = true;
        fb->editor_mode = true;
        fb->md_sel_active = false;   /* rendered-text selection is read-mode only */
        fb->cursor_line = 0;
        fb->cursor_col = 0;
        fb->view_scroll = 0;         /* the raw editor scrolls by line, not px */
        fb->view_scroll_px = 0.0f;
        fb->view_scroll_x = 0;
        fb->hscroll_accum_px = 0.0f;
        fb->vscroll_accum_px = 0.0f;
    }
}

FbViewerToggle fb_viewer_toggle_kind(const FileBrowser *fb) {
    if (!fb) return FB_TOGGLE_NONE;
    if (fb->view_mode == FVIEW_MARKDOWN) return FB_TOGGLE_MARKDOWN;
    if ((fb->view_mode == FVIEW_IMAGE || fb->view_mode == FVIEW_CODE) && fb_path_is_svg(fb->view_path))
        return FB_TOGGLE_SVG;
    return FB_TOGGLE_NONE;
}

/* The same document the other way round. Both directions are an open, so the
 * unsaved-edits guard speaks first (fb_discard_ok inside): drawn → source is
 * free, source → drawn refuses once if the markup was edited and not saved —
 * the drawing is made from the file, not from the buffer. */
void fb_viewer_toggle(FileBrowser *fb) {
    switch (fb_viewer_toggle_kind(fb)) {
    case FB_TOGGLE_MARKDOWN:
        fb_md_selection_clear(fb);
        fb_md_toggle_edit(fb);
        break;
    case FB_TOGGLE_SVG: {
        char path[FB_MAX_PATH];              /* the open rewrites view_path: a copy to read from */
        snprintf(path, sizeof path, "%s", fb->view_path);
        if (fb->view_mode == FVIEW_IMAGE) fb_text_open_start(fb, path, path, FVIEW_CODE);
        else                              fb_image_decode_start(fb, path);
        break;
    }
    case FB_TOGGLE_NONE:
        break;
    }
}

/* True when a markdown viewer is in editable (write) mode. */
bool fb_md_edit_active(const FileBrowser *fb) {
    return fb && fb->view_mode == FVIEW_MARKDOWN && fb->md_raw_mode && fb->editor_mode;
}

/* Tear down the viewer. `keep_graph` is set ONLY by the paired return-to-graph
 * close sites (which call fb_reenter_graph right after); every other caller
 * uses fb_close_viewer (keep_graph=false), which frees the graph. graph_return
 * is always cleared here so it can never outlive a single note close.
 *
 * Unconditional: whether the text may be thrown away is asked BEFORE this, by
 * fb_close_viewer_ex and by the open that replaces a document (fb_discard_ok). */
static void fb_close_viewer_now(FileBrowser *fb, bool keep_graph) {
    fb->pending_open_name[0] = '\0';
    fb->pending_open_edit = false;
    fb->pending_open_keep_graph = false;
    fb->pending_open_anchor[0] = '\0';
    editor_line_cache_drop(fb);
    if (!keep_graph) fb_graph_job_drop(fb);    /* the scan is no longer wanted; no join */
    free(fb->view_content);
    fb->view_content = NULL;
    fb->view_size = 0;
    fb->view_cap = 0;
    fb->view_edit_gen++;
    fb->view_disk = (FileIdentity){0};
    fb->modified = false;                      /* there is no text left to be unsaved */
    unsaved_guard_disarm(&fb->guard);
    if (fb->md_images) { md_image_cache_destroy(fb->md_images); fb->md_images = NULL; }
    if (fb->md_embeds) { md_embed_cache_destroy(fb->md_embeds); fb->md_embeds = NULL; }
    if (fb->md_arena.base) { arena_destroy(&fb->md_arena); }
    fb_md_parse_cancel(fb); free(fb->md_src); fb->md_src = NULL;
    fb->md_doc = NULL;
    fb->md_raw_mode = false;
    fb->editor_mode = false;   /* leaving the viewer also leaves edit mode */
    /* The doc backing any active selection is gone; without this the next
     * Cmd+C (after the viewer reopens on a non-Markdown file or the user is
     * back in the file list) still routes through the MD copy path. */
    fb->md_sel_active = false;
    /* Release the per-render scratch buffers too — same helper the
     * open-a-non-markdown-file path uses. */
    md_scratch_release(fb);
    /* Find/Replace state is per-document; drop it with the buffer. */
    free(fb->find_matches);
    fb->find_matches = NULL;
    fb->find_match_count = 0;
    fb->find_match_cap = 0;
    fb->find_active = false;
    fb->find_replace_mode = false;
    fb->find_query_len = 0; fb->find_query[0] = '\0';
    fb->find_replace_len = 0; fb->find_replace[0] = '\0';
    fb->find_current = -1;
    free(fb->ac_items);
    fb->ac_items = NULL;
    fb->ac_count = fb->ac_cap = 0;
    fb->ac_active = false;
    fb_image_state_reset(fb);
    /* Preserve the graph (and its pan/zoom/layout) ONLY for the paired
     * return-to-graph close; every other caller frees it. graph_return is
     * cleared unconditionally so a stale flag can't leak the graph or teleport
     * a later Esc into a dead graph. */
    if (!keep_graph) {
        if (fb->graph) { md_graph_free(fb->graph); fb->graph = NULL; }
        fb->graph_fitted    = false;
        fb->graph_hover     = -1;
        fb->graph_focus_node = -1;
        fb->graph_focus_t   = 0.0f;
        fb->graph_drag_node = -1;
        fb->graph_panning   = false;
    }
    fb->graph_return = false;
    fb->view_mode = FVIEW_NONE;
    fb->view_path[0] = '\0';
    fb->code_lang = SYN_LANG_NONE;
    fb_syn_invalidate(fb);

    /* Drop all undo state with the closed buffer. */
    fb_editor_reset_undo(fb);

    /* If the list was narrowed to .md-only for graph mode, repopulate the full
     * folder listing now that the graph/viewer is closed. Skipped on the
     * keep-graph path — fb_reenter_graph rebuilds the notes-only list, so the
     * scan here would be wasted. fb_navigate clears the flag on success; only
     * force it false if the reload can't run. */
    if (fb->md_only_entries && !keep_graph) {
        if (!(fb->source == FB_SOURCE_LOCAL && fb->cwd[0] && fb_navigate(fb, fb->cwd)))
            fb->md_only_entries = false;
    }
}

/* Close the viewer, unless that would throw unsaved edits away for the first
 * time of asking: then nothing is closed, the notice says why, and the same
 * call again closes it. Returns whether it closed. */
bool fb_close_viewer_ex(FileBrowser *fb, bool keep_graph) {
    if (!fb) return false;
    if (!fb_discard_ok(fb, UNSAVED_CLOSE)) return false;
    fb_close_viewer_now(fb, keep_graph);
    return true;
}

/* Default close: free the graph and any md-only narrowing. The keep-graph
 * variant above is for the paired return-to-graph close only. */
bool fb_close_viewer(FileBrowser *fb) {
    return fb_close_viewer_ex(fb, false);
}

/* =========================================================================
 * Rendering — Sidebar file list
 * ========================================================================= */

/* UI text rendering with explicit step size (0 = use font cell_width) */
static f32 g_ui_step = 0;

static void draw_text_clipped(Renderer *r, const char *text, f32 x, f32 y,
                               Color fg, f32 max_w) {
    if (!text) return;
    /* Pre-rasterize any uncached non-ASCII codepoints synchronously.
     * The async path returns a space placeholder that only resolves on
     * the next frame, but the UI is event-driven — without a follow-up
     * event the file browser keeps re-rendering the placeholder and
     * Turkish/Cyrillic/CJK filenames look like they have empty cells. */
    font_warm_text_glyphs(&r->font, text);

    f32 step = g_ui_step > 0 ? g_ui_step : r->font.cell_width;
    if (step <= 0.0f || max_w < step) return;

    /* Truncation gets an ellipsis, because a hard cut misleads:
     * "browser_ui.c" clipped mid-extension reads as a file actually named
     * "browser_ui." and "agent_icons.c/.h/.png" all collapse to the same
     * three identical rows. One cell of the budget is reserved for the '…'
     * when the text won't fit whole. Codepoints occupy one cell each here,
     * so counting them is the width measurement. */
    u32 cps = 0;
    for (const u8 *q = (const u8 *)text; *q; q++)
        if ((*q & 0xC0) != 0x80) cps++;
    bool trunc = ((f32)cps * step > max_w + 0.5f);
    f32 limit  = x + max_w - (trunc ? step : 0.0f);

    f32 cx = x;
    const u8 *p = (const u8 *)text;
    while (*p) {
        if (cx + step > limit) break;
        u32 cp;
        u32 consumed = utf8_decode(p, 4, &cp);
        if (consumed == 0) { p++; continue; }
        if (cp >= 32) {
            renderer_push_glyph(r, cx, y, cp, fg);
            cx += step;
        }
        p += consumed;
    }
    if (trunc && *p) renderer_push_glyph(r, cx, y, 0x2026, fg);   /* … */
}

/* Code points in `s`: the cells draw_text_clipped steps through. */
static usize fb_text_cols(const char *s) {
    return s ? utf8_len((const u8 *)s, strlen(s)) : 0;
}

/* Byte offset of the `n`-th code point (clamped to the terminator), so a
 * string trimmed from the left starts on a character boundary. */
static usize fb_utf8_skip(const char *s, usize n) {
    const u8 *p = (const u8 *)s;
    while (n > 0 && *p) {
        u32 cp;
        u32 consumed = utf8_decode(p, 4, &cp);
        p += consumed ? consumed : 1;
        n--;
    }
    return (usize)((const char *)p - s);
}

/* The width draw_text_clipped gives `text` inside `max_w`: whole cells. */
static f32 fb_text_drawn_w(const char *text, f32 step, f32 max_w) {
    if (step <= 0.0f || max_w < step) return 0.0f;
    f32 whole = (f32)fb_text_cols(text) * step;
    f32 room  = floorf(max_w / step) * step;
    return whole < room ? whole : room;
}

/* draw_text_clipped that loses the START instead of the end: an ellipsis,
 * then as much of the end of `text` as fits, so a path keeps its nearest
 * folders. */
static void draw_text_tail(Renderer *r, const char *text, f32 x, f32 y, Color fg, f32 max_w) {
    f32 step = g_ui_step > 0 ? g_ui_step : r->font.cell_width;
    if (!text || step <= 0.0f || max_w < step) return;
    usize cols = fb_text_cols(text);
    usize fits = (usize)(max_w / step);
    if (cols <= fits) { draw_text_clipped(r, text, x, y, fg, max_w); return; }
    if (fits < 2) return;
    font_warm_text_glyphs(&r->font, text);
    renderer_push_glyph(r, x, y, 0x2026, fg);   /* … */
    draw_text_clipped(r, text + fb_utf8_skip(text, cols - (fits - 1)), x + step, y, fg,
                      max_w - step);
}

/* Map the semantic file-type kind (fb_icon_for) onto the monoline vector icon
 * set — what a folder wears, and what a file wears when neither Seti nor a
 * logo can be drawn for it. A logo kind lands on the code mark here, which is
 * only ever its fallback: fb_file_icon_draw paints the PNG first. */
static IconKind fb_minimal_icon(FbAssetIconKind kind) {
    switch (kind) {
    case FB_ASSET_ICON_FOLDER:   return ICON_FT_FOLDER;
    case FB_ASSET_ICON_NONE:
    case FB_ASSET_ICON_FILE:     return ICON_FT_DOC;
    case FB_ASSET_ICON_TEXT:     return ICON_FT_DOC_TEXT;
    case FB_ASSET_ICON_IMAGE:    return ICON_FT_IMAGE;
    case FB_ASSET_ICON_VIDEO:    return ICON_FT_FILM;
    case FB_ASSET_ICON_AUDIO:    return ICON_FT_MUSIC;
    case FB_ASSET_ICON_ARCHIVE:  return ICON_FT_ARCHIVE;
    case FB_ASSET_ICON_PDF:      return ICON_FT_PDF;
    case FB_ASSET_ICON_TERMINAL: return ICON_FT_TERMINAL;
    case FB_ASSET_ICON_GEAR:     return ICON_GEAR;
    case FB_ASSET_ICON_GIT:      return ICON_FT_BRANCH;
    case FB_ASSET_ICON_LOCK:     return ICON_FT_LOCK;
    default:                     return ICON_FT_CODE;  /* all LANG_* logos */
    }
}

FbFileIcon fb_file_icon_kind(const char *name, bool is_dir) {
#ifdef HAVE_FB_ICON
    /* A file wears Seti's icon. One Seti has no rule for keeps the logo it had
     * (.astro, .nix, …) when there is one and takes Seti's default mark
     * otherwise, so a folder of unknown files still reads as one set. Seti is
     * asked FIRST: this runs per row per painted frame, it answers most names,
     * and the older table behind fb_icon_for costs several times as much per
     * name. */
    if (!is_dir && name) {
        SetiIcon si;
        if (!seti_icon_for(name, &si)) {
            FbAssetIconKind logo = fb_icon_for(name, false).asset;
            if (fb_asset_icon_is_logo(logo)) return logo;
        }
        return FB_FILE_ICON_SETI | ((u32)si.color << 8) | si.icon;
    }
#endif
    FbIcon ic = fb_icon_for(name, is_dir);
    /* A directory whose lookup yields no kind (stubbed fb_icon_for) must
     * still read as a folder, not a document. */
    if (is_dir && ic.asset == FB_ASSET_ICON_NONE) return FB_ASSET_ICON_FOLDER;
    /* No logo and no entry in the icon table, but the highlighter knows the
     * language (.astro, .zig, .nix, …): that is source, and it reads as code
     * rather than as a blank document. The scanner's table is the wider one
     * and it is the one that actually opens the file as code. */
    if (!is_dir && name && (ic.asset == FB_ASSET_ICON_NONE || ic.asset == FB_ASSET_ICON_FILE) &&
        syntax_lang_for_path(name, strlen(name)) != SYN_LANG_NONE)
        return FB_ASSET_ICON_CODE;
    return ic.asset;
}

f32 fb_file_icon_ink_left(FbFileIcon icon, f32 size) {
    f32 l = 0.0f;
    /* A Seti glyph is centred in its box and its width is the glyph's own (a
     * "C" is half as wide as a "TS"), so hanging each one off the rail by its
     * ink would leave their centres ragged. Its box goes where the folder's
     * does instead: the column of file marks is centred on the folders'. */
    if (icon & FB_FILE_ICON_SETI) {
        icon_ink_insets(ICON_FT_FOLDER, size, &l, NULL);
        return l;
    }
    FbAssetIconKind kind = (FbAssetIconKind)icon;
#ifdef HAVE_FB_ICON
    if (fb_asset_icon_is_logo(kind)) {
        fb_asset_icon_ink_insets(kind, (i32)floorf(size + 0.5f), &l, NULL);
        return l;
    }
#endif
    icon_ink_insets(fb_minimal_icon(kind), size, &l, NULL);
    return l;
}

void fb_file_icon_draw(Renderer *r, FbFileIcon icon, f32 x, f32 y,
                       f32 size, Color tint) {
    if (!r || size < 1.0f) return;
    FbAssetIconKind kind = (icon & FB_FILE_ICON_SETI) ? FB_ASSET_ICON_FILE
                                                      : (FbAssetIconKind)icon;
#ifdef HAVE_FB_ICON
    if (icon & FB_FILE_ICON_SETI) {
        /* The mask is rasterised at this exact pixel size and tinted in the
         * draw, keyed on (icon, size) like a Lucide mask. Its colour is
         * Seti's, judged against the window's surface — the one every place
         * this is drawn sits on — and only the caller's alpha applies. */
        i32 ps = (i32)floorf(size + 0.5f);
        u32 si = icon & 0xffu;
        const u8 *px = seti_icon_mask(si, ps);
        if (px) {
            Color c = seti_icon_color((icon >> 8) & 0xffu, r->text_surface);
            c.a = tint.a;
            renderer_draw_image_tinted(r, seti_icon_cache_key(si, ps), 0, px, ps, ps,
                                       floorf(x + 0.5f), floorf(y + 0.5f),
                                       (f32)ps, (f32)ps, c);
            return;
        }
    }
    if (fb_asset_icon_is_logo(kind)) {
        /* Exact-size raster: the Metal image path has no mipmaps, so a master
         * minified on the GPU aliases (fb_asset_icons.h). Pixel-snapped for
         * the same reason icon_draw snaps its quad. */
        i32 ps = (i32)floorf(size + 0.5f);
        i32 iw = 0, ih = 0;
        const u8 *px = fb_asset_icon_rgba_scaled(kind, ps, &iw, &ih);
        if (px) {
            /* A silhouette was re-inked to white at decode and takes the
             * caller's colour; a brand mark keeps its own and takes only the
             * alpha. Keyed on (kind, size), not on the buffer: the cache may
             * drop and re-make these pixels. */
            Color t = fb_asset_icon_is_mask(kind)
                    ? tint : (Color){ 1.0f, 1.0f, 1.0f, tint.a };
            renderer_draw_image_tinted(r, fb_asset_icon_cache_key(kind, ps), 0, px, iw, ih,
                                       floorf(x + 0.5f), floorf(y + 0.5f),
                                       (f32)ps, (f32)ps, t);
            return;
        }
    }
#endif
    icon_draw(r, fb_minimal_icon(kind), x, y, size, tint);
}

/* Colours of the explorer panel, from the three its caller hands it. */
typedef struct {
    Color body;     /* names */
    Color strong;   /* the folder's own name, a selected row's name */
    Color muted;    /* hidden entries, the parent path, chevrons, folder icons */
    Color faint;    /* sizes, the empty-folder line */
} FbPanelInk;

static f32 fb_ink_lum(Color c) { return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b; }

static FbPanelInk fb_panel_ink(Color fg, Color bg) {
    bool dark = fb_ink_lum(fg) > fb_ink_lum(bg);
    f32 to = dark ? 1.0f : 0.0f;
    FbPanelInk k;
    k.body   = (Color){ fg.r, fg.g, fg.b, 1.0f };
    k.strong = (Color){ fg.r + (to - fg.r) * 0.45f, fg.g + (to - fg.g) * 0.45f,
                        fg.b + (to - fg.b) * 0.45f, 1.0f };
    k.muted  = (Color){ fg.r, fg.g, fg.b, 0.58f };
    k.faint  = (Color){ fg.r, fg.g, fg.b, 0.42f };
    return k;
}

/* The rows a selected entry's level spans: its parent's children and their
 * subtrees, whose indent guide at `level` is drawn brighter. */
static void fb_active_guide(const FileBrowser *fb, i32 first, i32 *level, i32 *lo, i32 *hi) {
    *level = -1; *lo = 0; *hi = -1;
    i32 s = fb->selected;
    if (s < first || s >= fb->entry_count || fb->entries[s].depth <= 0) return;
    i32 d = fb->entries[s].depth;
    i32 a = s, b = s;
    while (a - 1 >= first && fb->entries[a - 1].depth >= d) a--;
    while (b + 1 < fb->entry_count && fb->entries[b + 1].depth >= d) b++;
    *level = d - 1; *lo = a; *hi = b;
}

void fb_render_sidebar(FileBrowser *fb, Renderer *r, f32 x, f32 y, f32 w, f32 h,
                       const Color *bg, const Color *fg, const Color *sel,
                       const Color *dim, const Color *accent, f32 dpi,
                       f32 hover_x, f32 hover_y) {
    /* Before the early-out: a browser whose first remote listing is still in
     * flight has no entries yet, and this is the call that installs them. */
    fb_poll_async(fb);
    if (!fb->open || !fb->entries) return;
    f32 cw = 8.0f * dpi;
    f32 ch = 16.0f * dpi;
    g_ui_step = cw;
    renderer_set_ui_scale(r, cw, ch);
    (void)sel; (void)accent;

    /* No body fill: `bg` is the panel's surface, painted by the caller. Rules
     * follow its alpha so they fade with a translucent window. */
    FbPanelInk ink = fb_panel_ink(*fg, *bg);
    Color base = { bg->r, bg->g, bg->b, 1.0f };
    f32 line = fmaxf(1.0f, floorf(CHROME_LINE_PT * dpi));
    Color rule = panel_wash(base, *dim, 0.75f);
    rule.a *= bg->a;

    /* ---- header: the folder's name, its parent path, three buttons ------- */
    f32 hdr_h = fb_list_hdr_h(dpi);
    f32 hdr_cy = y + hdr_h * 0.5f;
    renderer_draw_rect(r, x, y + hdr_h - line, w, line, rule);

    f32 btn_w = FB_HDR_BTN_W_CELLS * cw;
    f32 hdr_icon = ui_icon_box_px(FB_HDR_ICON_PT, dpi);
    f32 close_ink_r = 0.0f;
    icon_ink_insets(ICON_CLOSE, hdr_icon, NULL, &close_ink_r);
    f32 close_x   = fb_hdr_close_x(x, w, cw, hdr_icon, close_ink_r, dpi);
    f32 refresh_x = close_x - btn_w;
    f32 up_x      = refresh_x - btn_w;
    f32 chip      = fminf(ui_icon_box_px(FB_PANEL_BTN_PT, dpi), fminf(btn_w, hdr_h - 4.0f * dpi));

    struct { f32 bx; i32 icon; bool is_close; } hbtns[3] = {
        { up_x,      ICON_UP,      false },
        { refresh_x, ICON_REFRESH, false },
        { close_x,   ICON_CLOSE,   true  },
    };
    for (i32 b = 0; b < 3; b++) {
        if (!ui_hover_hit(hbtns[b].bx, y, btn_w, hdr_h, hover_x, hover_y)) continue;
        Color c = hbtns[b].is_close ? panel_wash(base, (Color){ 0.90f, 0.36f, 0.36f, 1.0f }, 0.22f)
                                    : panel_wash(base, *fg, 0.11f);
        renderer_draw_rrect_simple(r, floorf(hbtns[b].bx + (btn_w - chip) * 0.5f),
                                   floorf(hdr_cy - chip * 0.5f), chip, chip, c,
                                   FB_PANEL_BTN_RADIUS_PT * dpi);
    }
    renderer_flush_rects(r);
    renderer_flush_rrects(r);
    for (i32 b = 0; b < 3; b++) {
        bool hot = ui_hover_hit(hbtns[b].bx, y, btn_w, hdr_h, hover_x, hover_y);
        Color ic = !hot ? ink.muted
                 : hbtns[b].is_close ? (Color){ 0.98f, 0.52f, 0.52f, 1.0f } : ink.strong;
        icon_draw(r, hbtns[b].icon, floorf(hbtns[b].bx + (btn_w - hdr_icon) * 0.5f),
                  floorf(hdr_cy - hdr_icon * 0.5f), hdr_icon, ic);
    }

    {
        /* The folder's own name first and in full where it fits; the path
         * above it after, dimmed, losing its START when short of room, so the
         * nearest parents are what stays. */
        const char *cwd = fb->cwd;
        const char *home = getenv("HOME");
        char short_cwd[FB_MAX_PATH];
        usize hl = home ? strlen(home) : 0;
        if (hl > 1 && strncmp(cwd, home, hl) == 0 && (cwd[hl] == '/' || cwd[hl] == '\0'))
            snprintf(short_cwd, sizeof short_cwd, "~%s", cwd + hl);
        else
            snprintf(short_cwd, sizeof short_cwd, "%s", cwd);
        usize scl = strlen(short_cwd);
        if (scl > 1 && short_cwd[scl - 1] == '/') short_cwd[--scl] = '\0';
        char parent[FB_MAX_PATH] = "";
        const char *leaf = short_cwd;
        for (usize i = scl; i > 0; i--) {
            if (short_cwd[i - 1] != '/') continue;
            if (i > 1 && i < scl) {
                usize pl = i - 1 < sizeof parent - 1 ? i - 1 : sizeof parent - 1;
                memcpy(parent, short_cwd, pl);
                parent[pl] = '\0';
                leaf = short_cwd + i;
            } else if (i == 1 && scl > 1) {
                snprintf(parent, sizeof parent, "/");
                leaf = short_cwd + 1;
            }
            break;
        }

        f32 fx = x + FB_PANEL_PAD_PT * dpi;
        f32 fico_ink = 0.0f;
        icon_ink_insets(ICON_FT_FOLDER, hdr_icon, &fico_ink, NULL);
        icon_draw(r, ICON_FT_FOLDER, floorf(fx - fico_ink), floorf(hdr_cy - hdr_icon * 0.5f),
                  hdr_icon, ink.muted);
        f32 tx = fx + hdr_icon + 7.0f * dpi;
        f32 ty = hdr_cy - ch * 0.5f;
        f32 room = up_x + (btn_w - chip) * 0.5f - 6.0f * dpi - tx;
        if (room >= cw) {
            draw_text_clipped(r, leaf, tx, ty, ink.strong, room);
            f32 px = tx + fb_text_drawn_w(leaf, cw, room) + cw;
            f32 prem = tx + room - px;
            if (parent[0] && prem > 3.0f * cw)
                draw_text_tail(r, parent, px, ty, ink.muted, prem);
        }
    }
    renderer_flush_rects(r);
    renderer_flush_glyphs(r);

    /* ---- rows ------------------------------------------------------------ */
    f32 row_h = fb_list_row_h(dpi);
    f32 list_y = y + hdr_h;
    f32 viewport_h = h - hdr_h;
    i32 first = fb_list_first(fb);
    i32 rows = fb->entry_count - first;

    fb->content_height = (f32)rows * row_h;
    fb->viewport_height = viewport_h;
    fb->list_row_h = row_h;
    if (fb->scroll_offset_px == 0.0f && fb->scroll_offset > 0)
        fb->scroll_offset_px = (f32)fb->scroll_offset * row_h;
    f32 max_scroll = fmaxf(0.0f, fb->content_height - viewport_h);
    if (fb->scroll_offset_px < 0) fb->scroll_offset_px = 0;
    if (fb->scroll_offset_px > max_scroll) fb->scroll_offset_px = max_scroll;

    i32 r0 = (i32)(fb->scroll_offset_px / row_h);
    i32 r1 = r0 + (i32)(viewport_h / row_h) + 2;
    if (r0 < 0) r0 = 0;
    if (r1 > rows) r1 = rows;
    f32 sub_offset = fb->scroll_offset_px - (f32)r0 * row_h;

    /* The worker's finished stats, before the rows that asked for them are
     * drawn: a row painted this frame shows the figure it asked for last
     * frame, never a stat taken on the draw path. */
    fb_stat_poll(fb);
    /* The rows are clipped to the LIST, not to the section. The list scrolls
     * by the pixel, so the first row is usually part-way above `list_y` — and
     * with only the caller's clip (header + list) that part would be painted
     * over the header. Scissors do not nest: the caller's clip is this
     * function's own rect, and it is put back after the rows, whose glyphs
     * are flushed before that. */
    renderer_push_scissor(r, x, list_y, w, viewport_h);

    f32 pad    = FB_PANEL_ROW_PAD_PT * dpi;
    f32 indent = FB_PANEL_INDENT_PT * dpi;
    f32 chev   = ui_icon_box_px(FB_PANEL_CHEV_PT, dpi);
    f32 icon   = ui_icon_box_px(FB_PANEL_ICON_PT, dpi);
    f32 icon_dx = (FB_PANEL_CHEV_PT + FB_PANEL_CHEV_GAP_PT) * dpi;
    f32 name_dx = icon_dx + FB_PANEL_ICON_PT * dpi + FB_PANEL_ICON_GAP_PT * dpi;
    bool show_size = w >= FB_PANEL_SIZE_MIN_W_PT * dpi;
    i32 act_level, act_lo, act_hi;
    fb_active_guide(fb, first, &act_level, &act_lo, &act_hi);

    /* Pass 1, flat fills: row states and indent guides. A row's state changes
     * only its fill, and only by a step of the text colour: the selected row
     * is a shade above a hovered one, with no rim and no accent, so a click
     * marks where the keys will act without reading as a highlight. */
    Color hov_bg   = panel_wash(base, *fg, 0.045f);
    Color sel_bg   = panel_wash(base, *fg, 0.075f);
    Color guide    = panel_wash(base, *fg, 0.10f);
    Color guide_on = panel_wash(base, *fg, 0.26f);
    for (i32 ri = r0; ri < r1; ri++) {
        i32 i = first + ri;
        const FileEntry *e = &fb->entries[i];
        f32 ey = list_y + (f32)(ri - r0) * row_h - sub_offset;
        if (ey + row_h < list_y || ey > list_y + viewport_h) continue;
        bool in_sel = fb_sel_has(fb, i) || i == fb->selected;
        if (in_sel)                      renderer_draw_rect(r, x, ey, w, row_h, sel_bg);
        else if (i == fb->hover_entry)   renderer_draw_rect(r, x, ey, w, row_h, hov_bg);
        i32 depth = e->depth > 0 ? e->depth : 0;
        for (i32 lv = 0; lv < depth; lv++) {
            f32 gx = floorf(x + pad + (f32)lv * indent + chev * 0.5f);
            bool on = lv == act_level && i >= act_lo && i <= act_hi;
            renderer_draw_rect(r, gx, ey, line, row_h, on ? guide_on : guide);
        }
    }
    renderer_flush_rects(r);

    /* Pass 2: chevrons and icons draw now; names and sizes go to the glyph
     * batch flushed below, still under the list's clip. */
    for (i32 ri = r0; ri < r1; ri++) {
        i32 i = first + ri;
        FileEntry *e = &fb->entries[i];
        f32 ey = list_y + (f32)(ri - r0) * row_h - sub_offset;
        if (ey + row_h < list_y || ey > list_y + viewport_h) continue;
        f32 cy = ey + row_h * 0.5f;
        bool in_sel = fb_sel_has(fb, i) || i == fb->selected;
        bool hidden = e->name[0] == '.';
        f32 lx = x + pad + fminf((f32)(e->depth > 0 ? e->depth : 0) * indent,
                                 fmaxf(0.0f, w - 140.0f * dpi));

        if (e->is_dir) {
            Color cc = in_sel || i == fb->hover_entry ? ink.body : ink.muted;
            if (e->tree_pending) cc.a *= 0.6f;
            icon_draw(r, e->expanded ? ICON_CHEVRON_DOWN : ICON_CHEVRON_RIGHT,
                      floorf(lx), floorf(cy - chev * 0.5f), chev, cc);
        }

        /* A file's Seti icon in Seti's colour (only the alpha applies to it);
         * a folder is the monoline folder in the muted ink. The same call the
         * editor tab makes for its pill. */
        Color ic = e->is_dir ? ink.muted
                             : (Color){ fg->r, fg->g, fg->b, hidden ? 0.78f : 1.0f };
        if (e->is_dir && hidden) ic.a *= 0.75f;
        FbFileIcon ik = fb_file_icon_kind(e->name, e->is_dir);
        f32 ix = lx + icon_dx;
        fb_file_icon_draw(r, ik, floorf(ix - fb_file_icon_ink_left(ik, icon)),
                          floorf(cy - icon * 0.5f), icon, ic);

        f32 nx = lx + name_dx;
        bool sized = show_size && !e->is_dir;
        f32 name_room = x + w - nx - FB_PANEL_PAD_PT * dpi - (sized ? FB_PANEL_SIZE_COL_PT * dpi : 0.0f);
        Color nc = in_sel ? ink.strong : hidden ? ink.muted : ink.body;
        f32 ty = cy - ch * 0.5f;
        if (name_room > 0.0f) draw_text_clipped(r, e->name, nx, ty, nc, name_room);

        /* No figure until the stat completes: a temporary zero would read as
         * an empty file. */
        if (sized) fb_entry_ensure_stat(fb, e);
        if (sized && e->stat_known) {
            const char *sz = fb_format_size(e->size);
            f32 sw = fb_text_drawn_w(sz, cw, FB_PANEL_SIZE_COL_PT * dpi);
            draw_text_clipped(r, sz, x + w - FB_PANEL_PAD_PT * dpi - sw, ty, ink.faint, sw);
        }
    }

    /* An empty folder says so, where its first row would be. */
    if (rows == 0 && !fb_local_listing_active(fb) && !fb->sftp_list_job) {
        draw_text_clipped(r, "This folder is empty", x + pad + icon_dx,
                          list_y + (row_h - ch) * 0.5f + 4.0f * dpi,
                          ink.faint, w - pad - icon_dx - FB_PANEL_PAD_PT * dpi);
    }
    renderer_flush_rects(r);
    renderer_flush_glyphs(r);

    renderer_push_scissor(r, x, y, w, h);     /* back to the caller's clip (see above the rows) */

    /* Scrollbar: a thumb with no track, only while it has something to say —
     * while the pointer is over the panel or the list moves, fading out a
     * moment after (ui_autohide_alpha, anim.h). */
    if (fb->content_height > viewport_h && viewport_h > 0) {
        bool ptr_inside = ui_hover_hit(x, y, w, h, hover_x, hover_y);
        /* Every scroll source — wheel, keyboard row navigation, a navigate
         * that resets the offset — lands here as a changed offset, so this one
         * test covers all of them without each caller stamping the clock. */
        if (fb->scroll_offset_px != fb->sb_seen_scroll) {
            fb->sb_seen_scroll = fb->scroll_offset_px;
            fb->sb_active_t    = platform_time_sec();
        }
        if (ptr_inside) fb->sb_active_t = platform_time_sec();
        f32 sb_alpha = ui_autohide_alpha(fb->sb_active_t, ptr_inside);
        if (sb_alpha > 0.004f) {
            f32 bar_w = FB_PANEL_SCROLL_W_PT * dpi;
            f32 bar_x = x + w - bar_w - 2.0f * dpi;
            f32 thumb_h = fmaxf((viewport_h / fb->content_height) * viewport_h, 24.0f * dpi);
            f32 thumb_y = list_y + 2.0f * dpi +
                          (fb->scroll_offset_px / max_scroll) * (viewport_h - thumb_h - 4.0f * dpi);
            Color thumb = panel_wash(base, *fg, 0.28f);
            thumb.a *= bg->a * sb_alpha;
            renderer_draw_rrect_simple(r, bar_x, thumb_y, bar_w, thumb_h, thumb, bar_w * 0.5f);
            renderer_flush_rrects(r);
        }
    }

    renderer_flush_rects(r);
    renderer_flush_glyphs(r);
    renderer_reset_ui_scale(r);
    g_ui_step = 0;
}

/* =========================================================================
 * Rendering — File viewer (markdown + code)
 * ========================================================================= */

static void fb_md_draw_selection(FileBrowser *fb, Renderer *r, const Theme *theme);

/* =========================================================================
 * Graph view (FVIEW_GRAPH)
 * ========================================================================= */

bool fb_graph_active(const FileBrowser *fb) {
    return fb && fb->view_mode == FVIEW_GRAPH && fb->graph;
}

/* Rebuild the list sidebar from the graph's note set so it mirrors the graph
 * exactly — every .md node (recursively, across subfolders), shown as a path
 * relative to the scoped folder. Opening an entry resolves cwd + relative-name,
 * so notes in subfolders open correctly. The full folder listing is restored
 * by fb_close_viewer via the md_only_entries flag. */
static void fb_set_entries_from_graph(FileBrowser *fb) {
    if (!fb) return;
    fb_entries_reset(fb);
    fb->selected = -1;
    fb_sel_clear(fb);
    fb->scroll_offset = 0;
    fb->scroll_offset_px = 0.0f;
    fb->md_only_entries = true;
    if (!fb->graph) return;   /* empty folder → empty notes list */
    if (!fb->entries) {
        fb->entry_cap = 64;
        fb->entries = calloc((usize)fb->entry_cap, sizeof(FileEntry));
        if (!fb->entries) return;
    }
    /* Relative base = the graph's SCAN ROOT, not fb->cwd — the two can differ
     * (e.g. "Open Vault Graph" on a tab that was browsing elsewhere), and a
     * cwd-based strip would leave absolute paths that then fail to open via
     * the cwd+name join. */
    const char *root = fb->graph_root[0] ? fb->graph_root : fb->cwd;
    usize rootlen = strlen(root);
    i32 nc = md_graph_node_count(fb->graph);
    for (i32 i = 0; i < nc && fb->entry_count < FB_MAX_ENTRIES; i++) {
        if (md_graph_node_is_tag(fb->graph, i)) continue;
        const char *path = md_graph_node_path(fb->graph, i);
        if (!path || !path[0]) continue;
        const char *rel = path;
        if (rootlen && strncmp(path, root, rootlen) == 0) {
            rel = path + rootlen; while (*rel == '/') rel++;
        }
        if (!*rel) continue;
        if (fb->entry_count >= fb->entry_cap) {
            i32 newcap = fb->entry_cap * 2;
            if (newcap > FB_MAX_ENTRIES) newcap = FB_MAX_ENTRIES;
            FileEntry *grown = realloc(fb->entries, (usize)newcap * sizeof(FileEntry));
            if (!grown) break;
            fb->entries = grown; fb->entry_cap = newcap;
        }
        FileEntry *e = &fb->entries[fb->entry_count];
        memset(e, 0, sizeof *e);
        fb_entry_set_name(e, rel);
        e->is_dir = false;
        struct stat st;
        if (stat(path, &st) == 0) { e->size = (u64)st.st_size; e->mtime = (u64)st.st_mtime; }
        /* Already stat'ed, and this list's names are RELATIVE to the root —
         * fb_entry_ensure_stat would resolve them against fb->cwd, lstat the
         * wrong path, and overwrite a correct size with nothing. */
        e->stat_known = true;
        fb->entry_count++;
    }
    if (fb->entry_count > 1)
        qsort(fb->entries, (usize)fb->entry_count, sizeof(FileEntry), entry_cmp);
}

/* ---- The scan job --------------------------------------------------------
 *
 * md_graph_build walks the folder tree and parses every .md. It runs off the
 * main thread so a slow or network volume — or the macOS file-access prompt,
 * which holds the scanning syscall until the user answers — cannot freeze the
 * UI.
 *
 * OWNERSHIP. The worker is handed an FbGraphJob and touches nothing else,
 * because a FileBrowser moves: the docked sidebar's content is struct-copied
 * between AppState and its tab on every tab switch, and a tab dragged to
 * another window takes its browser along. A worker holding a browser's
 * address would finish into whatever browser lives there by then. The browser
 * holds a HANDLE (FileBrowser.graph_job); moving the browser moves the handle
 * and the job stays where it is.
 *
 * LIFETIME. Two references: the browser's handle, and the worker's once one is
 * running. Letting go never joins — a worker inside a syscall does not come
 * back for a cancel flag it only reads between files. Whoever releases last
 * frees the result if nobody took it.
 *
 * BOUND. At most FB_GRAPH_MAX_WORKERS scans run at once. Dropped scans whose
 * worker is still stuck are exactly what would pile up without it; a scan asked
 * for past the cap waits (not started) and fb_graph_poll starts it when a
 * worker ends. */
#define FB_GRAPH_MAX_WORKERS 4

typedef struct FbGraphJob {
    _Atomic i32   refs;      /* the browser's handle + the worker, once started */
    _Atomic bool  done;      /* worker -> main: `result` is final */
    _Atomic bool  cancel;    /* main -> worker: nobody wants this any more */
    bool          started;   /* main thread only: a worker was created for it */
    MdGraph      *result;    /* written by the worker BEFORE `done`; taken by the poll */
    char          dir[FB_MAX_PATH];
} FbGraphJob;

static _Atomic i32 g_graph_workers;

i32 fb_graph_jobs_live(void) { return atomic_load(&g_graph_workers); }
i32 fb_graph_jobs_cap(void)  { return FB_GRAPH_MAX_WORKERS; }

static void fb_graph_job_release(FbGraphJob *j) {
    if (!j) return;
    if (atomic_fetch_sub(&j->refs, 1) != 1) return;
    if (j->result) md_graph_free(j->result);    /* finished, and nobody took it */
    free(j);
}

static void *fb_graph_job_main(void *arg) {
#ifdef __APPLE__
    pthread_setname_np("liu-graph-build");
#else
    pthread_setname_np(pthread_self(), "liu-graph-build");
#endif
    FbGraphJob *j = (FbGraphJob *)arg;
    MdGraph *g = atomic_load(&j->cancel) ? NULL : md_graph_build_ex(j->dir, &j->cancel);
    j->result = g;                               /* published before the flag */
    atomic_store(&j->done, true);
    fb_graph_job_release(j);
    atomic_fetch_sub(&g_graph_workers, 1);       /* a waiting scan may start now */
    platform_wake_event_loop();                  /* the poll that adopts it, or starts the next */
    return NULL;
}

typedef enum { GRAPH_START_OK, GRAPH_START_WAIT, GRAPH_START_FAILED } GraphStart;

static GraphStart fb_graph_job_try_start(FbGraphJob *j) {
    if (j->started) return GRAPH_START_OK;
    i32 n = atomic_load(&g_graph_workers);
    do {
        if (n >= FB_GRAPH_MAX_WORKERS) return GRAPH_START_WAIT;
    } while (!atomic_compare_exchange_weak(&g_graph_workers, &n, n + 1));

    atomic_fetch_add(&j->refs, 1);               /* the worker's */
    pthread_attr_t attr;
    bool have_attr = pthread_attr_init(&attr) == 0;
    if (have_attr) pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t th;
    int rc = have_attr ? pthread_create(&th, &attr, fb_graph_job_main, j) : EAGAIN;
    if (have_attr) pthread_attr_destroy(&attr);
    if (rc != 0) {
        atomic_fetch_sub(&j->refs, 1);
        atomic_fetch_sub(&g_graph_workers, 1);
        return GRAPH_START_FAILED;
    }
    j->started = true;
    return GRAPH_START_OK;
}

/* Let go of the browser's scan, if it has one. Raises cancel and drops the
 * handle; it does NOT wait — the worker ends when it next looks at the flag (or
 * when the syscall it is in returns) and frees what it built. */
static void fb_graph_job_drop(FileBrowser *fb) {
    if (!fb || !fb->graph_job) return;
    FbGraphJob *j = fb->graph_job;
    fb->graph_job = NULL;
    atomic_store(&j->cancel, true);
    fb_graph_job_release(j);
}

bool fb_graph_scanning(const FileBrowser *fb) {
    return fb && fb->graph_job != NULL;
}

/* Put a finished scan's graph on screen. `g` may be NULL (no .md under the
 * folder, or the scan failed): the view shows its empty state — what matters
 * is that the spinner ends either way. */
static void fb_graph_install(FileBrowser *fb, MdGraph *g) {
    if (fb->graph) md_graph_free(fb->graph);
    fb->graph = g;
    fb->graph_fitted = false;
    fb->graph_user_zoomed = false;
    fb->graph_fit_zoom = 0.0f;
    fb->graph_pan_x = fb->graph_pan_y = 0.0f;
    fb->graph_zoom = 1.0f;
    fb->graph_hover = fb->graph_drag_node = -1;
    fb->gset_dirty = true;           /* re-apply user params: build resets them to 1.0 */
    fb_set_entries_from_graph(fb);   /* sidebar mirrors the graph notes */
}

bool fb_graph_poll(FileBrowser *fb) {
    FbGraphJob *j = fb ? fb->graph_job : NULL;
    if (!j) return false;
    if (!j->started) {
        GraphStart st = fb_graph_job_try_start(j);
        if (st != GRAPH_START_FAILED) return false;      /* running now, or still waiting */
        /* No thread to be had: scan here, as the pre-worker code did. Rare. */
        fb->graph_job = NULL;
        MdGraph *g = md_graph_build(j->dir);
        fb_graph_job_release(j);
        fb_graph_install(fb, g);
        return true;
    }
    if (!atomic_load(&j->done)) return false;
    fb->graph_job = NULL;
    MdGraph *g = j->result;          /* the worker is past its last write */
    j->result = NULL;
    fb_graph_job_release(j);
    fb_graph_install(fb, g);
    return true;
}

/* Scan `dir` for this browser; the graph view shows a spinner until the poll
 * adopts the result. Whatever scan the browser had is let go first. */
static void fb_graph_build_start(FileBrowser *fb, const char *dir) {
    fb_graph_job_drop(fb);
    if (fb->graph) { md_graph_free(fb->graph); fb->graph = NULL; }
    FbGraphJob *j = calloc(1, sizeof *j);
    if (j) {
        atomic_init(&j->refs, 1);                /* the browser's handle */
        atomic_init(&j->done, false);
        atomic_init(&j->cancel, false);
        snprintf(j->dir, sizeof j->dir, "%s", dir);
        if (fb_graph_job_try_start(j) != GRAPH_START_FAILED) {
            fb->graph_job = j;                   /* running, or waiting for a worker */
            return;
        }
        fb_graph_job_release(j);
    }
    /* No job or no thread: scan synchronously and show it now. */
    fb->graph = md_graph_build(dir);             /* may be NULL (no .md under dir) */
}

/* Toggle the knowledge graph, building it from `root` (e.g. the notes Vault)
 * rather than the file browser's cwd. NULL/empty root falls back to fb->cwd. */
bool fb_toggle_graph_root(FileBrowser *fb, const char *root) {
    if (!fb) return false;
    if (fb->view_mode == FVIEW_GRAPH) { fb_close_viewer(fb); return false; }
    if (!root || !root[0]) root = fb->cwd;
    /* Leaving any open file viewer first — and not at all when that close
     * was refused (unsaved edits): the graph must not be put over the note. */
    if (fb->view_mode != FVIEW_NONE && !fb_close_viewer(fb)) return false;
    fb->graph_pan_x = fb->graph_pan_y = 0.0f;
    fb->graph_zoom = 1.0f;
    fb->graph_fitted = false;
    fb->graph_user_zoomed = false;
    fb->graph_fit_zoom = 0.0f;
    fb->graph_raster_gen = 1;
    fb->graph_hover = fb->graph_drag_node = -1;
    fb->graph_panning = false;
    fb->graph_moved = false;
    /* Open with a narrow file-list sidebar so the graph fills the window, even
     * if this tab's viewer divider was previously dragged wide for a file. The
     * tab layer consumes this on the next render (it owns fb_viewer_ratio). */
    fb->reset_split_narrow = true;
    fb->graph_return = false;   /* entering the graph view fresh */
    snprintf(fb->graph_root, sizeof fb->graph_root, "%s", root);
    fb->view_mode = FVIEW_GRAPH;
    snprintf(fb->view_path, sizeof fb->view_path, "%s", root);
    /* cwd follows the graph root (mirrors fb_reenter_graph): the notes-only
     * sidebar names are relative to the root and open via cwd+name, so a
     * root != cwd caller (vault graph on a browsing tab) must not leave the
     * two diverged. */
    snprintf(fb->cwd, sizeof fb->cwd, "%s", root);
    /* fb_entries_reset, not `entry_count = 0`: FileEntry.name is a heap copy
     * (fb_entry_set_name) and this loop is the ONLY code that frees it. Zeroing
     * the count first does not merely skip the free — it makes every LATER
     * reset a no-op over those rows, so fb_set_entries_from_graph's own reset
     * and even fb_destroy walk zero entries and the names are unreachable from
     * that instant. */
    fb_entries_reset(fb);              /* sidebar fills once the build completes */
    fb_graph_build_start(fb, root);    /* scan off the main thread */
    return true;
}

/* Graph the file browser's current folder (the original cwd-rooted behaviour). */
bool fb_toggle_graph(FileBrowser *fb) {
    return fb_toggle_graph_root(fb, fb ? fb->cwd : NULL);
}

/* Re-scope the graph recursively to dir while keeping graph mode active.
 * Return true if the directory contains Markdown files; clear the graph when
 * it does not. */
bool fb_graph_rescope(FileBrowser *fb, const char *dir) {
    if (!fb || !dir || !dir[0]) return false;
    /* The graph takes the viewer from whatever document is in it. */
    if (!fb_discard_ok(fb, UNSAVED_CLOSE)) return false;
    /* Switching straight into FVIEW_GRAPH without going through
     * fb_close_viewer_ex, so drop any decoded image here too. Otherwise an
     * animated GIF viewed just before "Open Vault Graph" keeps
     * view_image_is_animated set and its whole RGBA buffer resident, and the
     * main loop's GIF pacing keeps selecting this browser as the animation
     * driver even though the viewer is now a graph. */
    fb_image_state_reset(fb);
    fb->graph_pan_x = fb->graph_pan_y = 0.0f;
    fb->graph_zoom = 1.0f;
    fb->graph_fitted = false;
    fb->graph_user_zoomed = false;
    fb->graph_fit_zoom = 0.0f;
    fb->graph_raster_gen = 1;
    fb->graph_hover = fb->graph_drag_node = -1;
    fb->graph_panning = false;
    fb->graph_moved = false;
    fb->graph_next_frame_at = 0.0;
    fb->graph_return = false;   /* entering the graph view fresh */
    /* Track the live graph's root so return-from-note restores THIS scope, not
     * the folder the graph was first opened on. */
    snprintf(fb->graph_root, sizeof fb->graph_root, "%s", dir);
    fb->view_mode = FVIEW_GRAPH;
    snprintf(fb->view_path, sizeof fb->view_path, "%s", dir);
    /* Same cwd-follows-root rule as fb_toggle_graph_root: rescope callers do
     * not all fb_navigate first ("Open Vault Graph" on an existing tab). */
    snprintf(fb->cwd, sizeof fb->cwd, "%s", dir);
    /* fb_entries_reset, not `entry_count = 0`: FileEntry.name is a heap copy
     * (fb_entry_set_name) and this loop is the ONLY code that frees it. Zeroing
     * the count first does not merely skip the free — it makes every LATER
     * reset a no-op over those rows, so fb_set_entries_from_graph's own reset
     * and even fb_destroy walk zero entries and the names are unreachable from
     * that instant. */
    fb_entries_reset(fb);              /* sidebar fills once the build completes */
    fb_graph_build_start(fb, dir);     /* scan off the main thread */
    return true;
}

/* True when the viewer is showing a note opened from the graph and the graph is
 * still alive — i.e. closing the note should return to the graph. */
bool fb_graph_can_return(const FileBrowser *fb) {
    return fb && fb->graph_return && fb->graph &&
           fb->view_mode != FVIEW_GRAPH;
}

/* Re-enter the preserved graph after a note opened from it is closed. The graph
 * object (and its pan/zoom/settled layout) was kept alive across the note, so
 * this resumes exactly where the user left off — only the list sidebar and the
 * scoped cwd/path are restored. Call AFTER fb_close_viewer (which, with
 * graph_return set, left the graph intact). */
void fb_reenter_graph(FileBrowser *fb) {
    if (!fb || !fb->graph) { if (fb) fb->graph_return = false; return; }
    /* The close this follows can be REFUSED (the note has unsaved edits, and
     * was asked for the first time). The note is then still open, and the
     * graph is not put over it; graph_return stays for the close that works. */
    if (fb_view_is_document(fb)) return;
    fb_image_state_reset(fb);   /* same reason as fb_graph_rescope */
    fb->graph_return = false;
    if (fb->graph_root[0]) {
        snprintf(fb->cwd, sizeof fb->cwd, "%s", fb->graph_root);
        snprintf(fb->view_path, sizeof fb->view_path, "%s", fb->graph_root);
    }
    fb->view_mode = FVIEW_GRAPH;
    fb->graph_next_frame_at = 0.0;
    fb_set_entries_from_graph(fb);   /* restore the notes-only list sidebar */
}

/* Distinct, evenly-spread colour per folder cluster (golden-angle hue), nudged
 * toward the theme node colour so it still sits in the palette. gi<0 → base. */
/* Small color helpers for the graph palette (theme-derived, polarity-aware). */
static inline Color gcol_blend(Color a, Color b, f32 t) {
    return (Color){ a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t,
                    a.b + (b.b - a.b) * t, 1.0f };
}
static inline Color gcol_lift(Color c, f32 t) {   /* push toward white by t */
    return (Color){ c.r + (1.0f - c.r) * t, c.g + (1.0f - c.g) * t,
                    c.b + (1.0f - c.b) * t, 1.0f };
}

/* Distinct per-component cluster hue (golden-angle rotation), blended toward
 * the node base. `light` themes use richer/darker hues so they contrast on a
 * white canvas; dark themes keep the brighter tone. */
static Color graph_group_color(i32 gi, Color base, bool light) {
    if (gi < 0) return base;
    f32 h = fmodf((f32)gi * 137.508f, 360.0f) / 60.0f;   /* 0..6 hue sector */
    f32 s = light ? 0.62f : 0.50f, v = light ? 0.70f : 0.95f;
    f32 mix = light ? 0.35f : 0.20f;                     /* base-blend weight */
    i32 hi = (i32)h; f32 f = h - (f32)hi;
    f32 p = v * (1.0f - s), q = v * (1.0f - s * f), t = v * (1.0f - s * (1.0f - f));
    f32 rr, gg, bb;
    switch (hi % 6) {
        case 0: rr=v; gg=t; bb=p; break;
        case 1: rr=q; gg=v; bb=p; break;
        case 2: rr=p; gg=v; bb=t; break;
        case 3: rr=p; gg=q; bb=v; break;
        case 4: rr=t; gg=p; bb=v; break;
        default:rr=v; gg=p; bb=q; break;
    }
    return (Color){ rr*(1.0f-mix) + base.r*mix,
                    gg*(1.0f-mix) + base.g*mix,
                    bb*(1.0f-mix) + base.b*mix, 1.0f };
}

/* Coarse screen-space occupancy grid for label decluttering. All coordinates
 * are VIEW-RELATIVE (md_graph_node_screen returns sx/sy without the +x,+y view
 * origin — that's added only when pushing glyphs). Greedy: the highest-priority
 * labels are placed first and reserve their box; lower ones yield on collision. */
typedef struct {
    u8  *cells;          /* cols*rows, 0=free 1=taken */
    i32  cols, rows;
    f32  cell;           /* px per cell */
    f32  w, h;           /* view rect size */
} LblGrid;

static inline i32 lbl_clampi(i32 v, i32 lo, i32 hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}
/* View-relative AABB → clamped cell spans; false if fully off the grid. */
static bool lbl_span(const LblGrid *g, f32 x0, f32 y0, f32 x1, f32 y1,
                     i32 *c0, i32 *c1, i32 *r0, i32 *r1) {
    if (x1 < 0 || y1 < 0 || x0 > g->w || y0 > g->h) return false;
    *c0 = lbl_clampi((i32)floorf(x0 / g->cell), 0, g->cols - 1);
    *c1 = lbl_clampi((i32)floorf(x1 / g->cell), 0, g->cols - 1);
    *r0 = lbl_clampi((i32)floorf(y0 / g->cell), 0, g->rows - 1);
    *r1 = lbl_clampi((i32)floorf(y1 / g->cell), 0, g->rows - 1);
    return true;
}
static bool lbl_box_free(const LblGrid *g, f32 x0, f32 y0, f32 x1, f32 y1) {
    i32 c0, c1, r0, r1;
    if (!lbl_span(g, x0, y0, x1, y1, &c0, &c1, &r0, &r1)) return true; /* off-grid: no conflict */
    for (i32 rr = r0; rr <= r1; rr++)
        for (i32 cc = c0; cc <= c1; cc++)
            if (g->cells[rr * g->cols + cc]) return false;
    return true;
}
static void lbl_box_mark(LblGrid *g, f32 x0, f32 y0, f32 x1, f32 y1) {
    i32 c0, c1, r0, r1;
    if (!lbl_span(g, x0, y0, x1, y1, &c0, &c1, &r0, &r1)) return;
    for (i32 rr = r0; rr <= r1; rr++)
        for (i32 cc = c0; cc <= c1; cc++)
            g->cells[rr * g->cols + cc] = 1;
}
/* Descending sort of packed (priority<<20 | index) keys. */
static int lbl_key_desc(const void *a, const void *b) {
    u64 ka = *(const u64 *)a, kb = *(const u64 *)b;
    return (ka < kb) - (ka > kb);
}

/* Apply sane defaults once (all multipliers = 1, filters on, uniform
 * colour for the default graph view). */
static void graph_settings_ensure(FileBrowser *fb) {
    if (fb->gset_inited) return;
    fb->gset.center = fb->gset.repel = fb->gset.link = fb->gset.linkdist = 1.0f;
    fb->gset.node_size = fb->gset.link_thickness = fb->gset.text_fade = 1.0f;
    fb->gset.show_arrows  = false;
    fb->gset.show_orphans = true;
    fb->gset.show_tags    = true;
    fb->gset.color_mode   = 0;            /* uniform, single-color mode */
    fb->graph_drag_slider = -1;
    fb->gset_dirty = true;                /* apply defaults to the sim on first frame */
    fb->gset_inited = true;
}

/* A node hidden by the active filters (orphans toggle / tags toggle). */
static bool graph_node_hidden(const FileBrowser *fb, i32 i) {
    if (md_graph_node_is_tag(fb->graph, i)) return !fb->gset.show_tags;
    if (!fb->gset.show_orphans && md_graph_node_degree(fb->graph, i) <= 0) return true;
    return false;
}

/* Hit test that ignores filtered-out nodes. The renderer skips hidden orphans
 * and tags, so they must not be hoverable / draggable / openable either. */
static i32 graph_hit_visible(const FileBrowser *fb, f32 rx, f32 ry, f32 w, f32 h) {
    i32 node = md_graph_hit(fb->graph, rx, ry, (i32)w, (i32)h,
                            fb->graph_pan_x, fb->graph_pan_y, fb->graph_zoom);
    return (node >= 0 && graph_node_hidden(fb, node)) ? -1 : node;
}

/* ---- Graph controls panel (Forces / Display / Filters) ---------------- */

typedef enum { GC_SLIDER, GC_TOGGLE } GCKind;
typedef struct {
    GCKind      kind;
    const char *label;
    f32         minv, maxv;
    f32        *fval;        /* slider value */
    bool       *bval;        /* toggle bool, or NULL when ival is used */
    i32        *ival;        /* toggle 0/1 (colour mode) */
    bool        is_force;    /* force slider → reheat sim on change */
    f32         ry, rh;      /* row top + height (x/width come from the panel) */
} GCtl;

#define GRAPH_PANEL_CTLS 11

/* Compute the gear button + panel rect + each control's row. Render and the
 * input handlers both call this so their geometry can never drift. */
static i32 graph_panel_layout(FileBrowser *fb, f32 dpi, GCtl *out,
                              f32 *gx, f32 *gy, f32 *gs,
                              f32 *px, f32 *py, f32 *pw, f32 *ph,
                              f32 *cx, f32 *cw) {
    f32 vx = fb->graph_view_x, vy = fb->graph_view_y, vw = fb->graph_view_w;
    f32 margin = 10.0f*dpi, gear = 26.0f*dpi, panw = 216.0f*dpi, pad = 12.0f*dpi;
    /* The viewer reserves the top VIEWER_TITLE_H strip for an (invisible-in-graph)
     * title bar + close button — keep the gear BELOW it, otherwise the click is
     * hit-tested as the viewer close button and shuts the graph instead. */
    f32 title_h = VIEWER_TITLE_H_PT * dpi;
    *gs = gear;
    *gx = vx + vw - margin - gear;  *gy = vy + title_h + 8.0f*dpi;
    *px = vx + vw - margin - panw;  *py = *gy + gear + 6.0f*dpi;
    *pw = panw;  *cx = *px + pad;  *cw = panw - 2.0f*pad;

    f32 header_h = 22.0f*dpi, slider_h = 30.0f*dpi, toggle_h = 24.0f*dpi;
    f32 yy = *py + pad;
    struct { GCKind k; const char *l; f32 a, b; f32 *f; bool *bb; i32 *ii; bool force; }
    d[GRAPH_PANEL_CTLS] = {
        {GC_SLIDER, "Center",     0, 2, &fb->gset.center,         NULL, NULL, true},
        {GC_SLIDER, "Repel",      0, 2, &fb->gset.repel,          NULL, NULL, true},
        {GC_SLIDER, "Link",       0, 2, &fb->gset.link,           NULL, NULL, true},
        {GC_SLIDER, "Link dist",  0, 2, &fb->gset.linkdist,       NULL, NULL, true},
        {GC_SLIDER, "Node size",  0, 2, &fb->gset.node_size,      NULL, NULL, true},
        {GC_SLIDER, "Link width", 0, 3, &fb->gset.link_thickness, NULL, NULL, false},
        {GC_SLIDER, "Text fade",  0, 2, &fb->gset.text_fade,      NULL, NULL, false},
        {GC_TOGGLE, "Arrows",     0, 0, NULL, &fb->gset.show_arrows,  NULL, false},
        {GC_TOGGLE, "Orphans",    0, 0, NULL, &fb->gset.show_orphans, NULL, false},
        {GC_TOGGLE, "Tags",       0, 0, NULL, &fb->gset.show_tags,    NULL, false},
        {GC_TOGGLE, "Clusters",   0, 0, NULL, NULL, &fb->gset.color_mode, false},
    };
    for (i32 i = 0; i < GRAPH_PANEL_CTLS; i++) {
        if (i == 0 || i == 4 || i == 8) yy += header_h;  /* section gap */
        out[i].kind = d[i].k; out[i].label = d[i].l;
        out[i].minv = d[i].a; out[i].maxv = d[i].b;
        out[i].fval = d[i].f; out[i].bval = d[i].bb; out[i].ival = d[i].ii;
        out[i].is_force = d[i].force;
        out[i].rh = (d[i].k == GC_SLIDER) ? slider_h : toggle_h;
        out[i].ry = yy; yy += out[i].rh;
    }
    *ph = (yy + pad) - *py;
    return GRAPH_PANEL_CTLS;
}

/* Slider track geometry inside a control row (shared render/hit). */
static void gc_track(const GCtl *c, f32 cx, f32 cw, f32 dpi,
                     f32 *tx, f32 *ty, f32 *tw, f32 *th) {
    *tx = cx; *tw = cw; *th = 4.0f*dpi; *ty = c->ry + c->rh - 11.0f*dpi;
}
/* Toggle checkbox geometry (right-aligned in the row). */
static void gc_box(const GCtl *c, f32 cx, f32 cw, f32 dpi,
                   f32 *bx, f32 *by, f32 *bs) {
    *bs = 16.0f*dpi; *bx = cx + cw - *bs; *by = c->ry + (c->rh - *bs)*0.5f;
}

/* Draw the gear button and, when expanded, the controls panel. */
static void graph_render_panel(FileBrowser *fb, Renderer *r, f32 dpi,
                               const Theme *theme) {
    GCtl c[GRAPH_PANEL_CTLS];
    f32 gx, gy, gs, px, py, pw, ph, cx, cw;
    graph_panel_layout(fb, dpi, c, &gx,&gy,&gs, &px,&py,&pw,&ph, &cx,&cw);

    Color fg     = theme ? theme->fg     : (Color){0.9f,0.9f,0.92f,1};
    Color dim    = (Color){fg.r, fg.g, fg.b, 0.55f};
    Color accent = theme ? theme->ansi[12] : (Color){0.40f,0.62f,1.0f,1};
    Color panelbg= theme ? theme->bg     : (Color){0.06f,0.06f,0.07f,1};
    Color border = theme ? theme->border : (Color){0.2f,0.2f,0.24f,1};
    border.a = border.a > 0.05f ? border.a : 1.0f;
    Color track  = (Color){fg.r, fg.g, fg.b, 0.14f};
    Color clear  = (Color){0,0,0,0};

    Color gbtn = fb->graph_settings_open ? accent : (Color){fg.r,fg.g,fg.b,0.10f};
    renderer_draw_rrect_simple(r, gx, gy, gs, gs, gbtn, 6.0f*dpi);
    renderer_flush_rrects(r);
    Color gico = fb->graph_settings_open ? (Color){0.05f,0.06f,0.08f,0.95f} : dim;
    f32 lx0 = gx + 6*dpi, lx1 = gx + gs - 6*dpi;
    f32 ly1 = gy + gs*0.40f, ly2 = gy + gs*0.62f;
    renderer_draw_rect(r, lx0, ly1, lx1-lx0, fmaxf(1.0f,1.5f*dpi), gico);
    renderer_draw_rect(r, lx0, ly2, lx1-lx0, fmaxf(1.0f,1.5f*dpi), gico);
    renderer_flush_rects(r);
    renderer_draw_rrect_simple(r, gx+gs*0.60f-2*dpi, ly1-2.0f*dpi, 4*dpi,4*dpi, gico, 2*dpi);
    renderer_draw_rrect_simple(r, gx+gs*0.36f-2*dpi, ly2-2.0f*dpi, 4*dpi,4*dpi, gico, 2*dpi);
    renderer_flush_rrects(r);

    if (!fb->graph_settings_open) return;

    renderer_draw_rrect_bordered(r, px, py, pw, ph, panelbg, border, fmaxf(1.0f,dpi),
                                 10*dpi,10*dpi,10*dpi,10*dpi, 18*dpi,0.30f,0,4*dpi);
    renderer_flush_rrects(r);

    for (i32 i = 0; i < GRAPH_PANEL_CTLS; i++) {
        if (c[i].kind == GC_SLIDER) {
            f32 tx,ty,tw,th; gc_track(&c[i], cx, cw, dpi, &tx,&ty,&tw,&th);
            f32 t = (*c[i].fval - c[i].minv) / (c[i].maxv - c[i].minv);
            if (t < 0) t = 0; if (t > 1) t = 1;
            renderer_draw_rrect_simple(r, tx, ty, tw, th, track, th*0.5f);
            if (t > 0) renderer_draw_rrect_simple(r, tx, ty, tw*t, th, accent, th*0.5f);
            f32 kr = 6.0f*dpi, kx = tx + tw*t;
            renderer_draw_rrect_simple(r, kx-kr, ty+th*0.5f-kr, kr*2, kr*2, fg, kr);
        } else {
            f32 bx,by,bs; gc_box(&c[i], cx, cw, dpi, &bx,&by,&bs);
            bool on = c[i].ival ? (*c[i].ival != 0) : (c[i].bval && *c[i].bval);
            if (on) renderer_draw_rrect_simple(r, bx,by,bs,bs, accent, 4*dpi);
            else    renderer_draw_rrect_bordered(r, bx,by,bs,bs, clear, border,
                                                 fmaxf(1.0f,dpi), 4*dpi,4*dpi,4*dpi,4*dpi, 0,0,0,0);
        }
    }
    renderer_flush_rrects(r);

    f32 tcw = 7.0f*dpi, tch = 13.0f*dpi;
    g_ui_step = tcw; renderer_set_ui_scale(r, tcw, tch);
    const char *secs[3] = { "FORCES", "DISPLAY", "FILTERS" };
    i32 secidx[3] = { 0, 4, 8 };
    for (int s = 0; s < 3; s++)
        draw_text_clipped(r, secs[s], cx, c[secidx[s]].ry - 17.0f*dpi, dim, cw);
    for (i32 i = 0; i < GRAPH_PANEL_CTLS; i++) {
        f32 ly = (c[i].kind == GC_SLIDER) ? c[i].ry
                                          : c[i].ry + (c[i].rh - tch)*0.5f;
        draw_text_clipped(r, c[i].label, cx, ly, fg, cw);
        if (c[i].kind == GC_SLIDER) {
            char val[16]; snprintf(val, sizeof val, "%.2f", *c[i].fval);
            f32 vw = (f32)strlen(val) * tcw;
            draw_text_clipped(r, val, cx + cw - vw, c[i].ry, dim, vw + tcw);
        }
    }
    renderer_flush_glyphs(r);
    renderer_reset_ui_scale(r); g_ui_step = 0;
}

/* `opacity` is the window's own alpha, threaded in from fb_render_viewer.
 * Graph tabs force viewer-only, and the canvas wash below covers 100% of the
 * content box — painting it at alpha 1 would punch a hole the size of the
 * whole pane through the window's translucency/blur. */
static void fb_render_graph(FileBrowser *fb, Renderer *r, f32 x, f32 y,
                            f32 w, f32 h, f32 dpi, const Theme *theme,
                            f32 opacity) {
    if (w < 4 || h < 4) return;
    if (opacity < 0.0f) opacity = 0.0f;
    if (opacity > 1.0f) opacity = 1.0f;
    graph_settings_ensure(fb);

    /* Adopt a finished scan (or start one that was waiting for a worker). */
    (void)fb_graph_poll(fb);
    /* While the scan runs, show a spinner and keep the frame loop alive so the
     * poll above fires every frame. The heavy I/O is on the worker — the UI
     * stays responsive even behind the macOS file-access permission prompt. */
    if (fb_graph_scanning(fb)) {
        f32 cw = 8.0f * dpi, ch = 16.0f * dpi;
        g_ui_step = cw; renderer_set_ui_scale(r, cw, ch);
        const char *msg = "Scanning notes\xe2\x80\xa6";
        f32 mw = 15.0f * cw;
        Color dim = theme ? (Color){ theme->sidebar_fg.r, theme->sidebar_fg.g,
                                     theme->sidebar_fg.b, 0.6f }
                          : (Color){ 0.6f, 0.6f, 0.6f, 0.6f };
        draw_text_clipped(r, msg, x + (w - mw) * 0.5f, y + h * 0.5f - ch * 0.5f, dim, mw + cw);
        renderer_reset_ui_scale(r); g_ui_step = 0;
        fb->graph_next_frame_at = platform_time_sec();  /* keep polling */
        return;
    }

    if (!fb->graph) {
        /* Folder filter scoped to a folder with no .md files. Tell the user
         * rather than showing a blank pane — they can navigate back up. */
        f32 cw = 8.0f * dpi, ch = 16.0f * dpi;
        g_ui_step = cw;                       /* draw_text_clipped glyph advance */
        renderer_set_ui_scale(r, cw, ch);
        const char *msg = "No markdown files in this folder";
        f32 mw = (f32)strlen(msg) * cw;
        Color dim = theme ? (Color){ theme->sidebar_fg.r, theme->sidebar_fg.g,
                                     theme->sidebar_fg.b, 0.6f }
                          : (Color){ 0.6f, 0.6f, 0.6f, 0.6f };
        draw_text_clipped(r, msg, x + (w - mw) * 0.5f, y + h * 0.5f - ch * 0.5f, dim, mw + cw);
        renderer_reset_ui_scale(r);
        g_ui_step = 0;
        return;
    }
    fb->graph_view_x = x; fb->graph_view_y = y;
    fb->graph_view_w = w; fb->graph_view_h = h;
    fb->graph_render_dpi = dpi;
    i32 iw = (i32)w, ih = (i32)h;

    /* Mirror the controls panel into the live simulation only when a control
     * actually changed (set by graph_settings_ensure + the panel handlers).
     * reheat on a force change is done by the panel handler, not here. */
    if (fb->gset_dirty) {
        md_graph_apply_params(fb->graph, fb->gset.center, fb->gset.repel,
                              fb->gset.link, fb->gset.linkdist, fb->gset.node_size);
        fb->gset_dirty = false;
    }

    if (!fb->graph_fitted) {
        f32 R = md_graph_world_radius(fb->graph);
        f32 fit = fminf((f32)iw, (f32)ih) * 0.44f / fmaxf(R, 1.0f);
        /* Entrance: open slightly zoomed out + transparent. The camera glide
         * below eases the zoom up to the fit while graph_intro fades the graph
         * in — a calm "open with animation" instead of a hard cut. */
        fb->graph_zoom = fit * 0.86f;
        fb->graph_pan_x = fb->graph_pan_y = 0.0f;
        fb->graph_fit_zoom = fit;
        fb->graph_fitted = true;
        fb->graph_intro_start = platform_time_sec();
    }

    /* Redraw-request accumulator for this frame. Every animation source, the
     * focus/intro fades included, raises this flag and the tail of this
     * function makes the single decision, so an idle sim+camera cannot
     * overwrite a fade's request and freeze it mid-animation. */
    bool want_anim = false;
    /* Feed the sim REAL elapsed time — md_graph_step integrates in fixed 1/60
     * sub-steps, so layout speed does not depend on the display refresh rate.
     */
    f64 step_now = platform_time_sec();
    f32 step_dt = (fb->graph_step_prev_t > 0.0)
                ? (f32)(step_now - fb->graph_step_prev_t) : (1.0f / 60.0f);
    fb->graph_step_prev_t = step_now;
    bool moving = md_graph_step(fb->graph, step_dt);
    /* Keep the layout framed while it's still moving, but GLIDE the camera
     * toward the fit instead of snapping each frame — snapping to a zoom that is
     * the reciprocal of a per-frame-changing world radius makes the view judder.
     * The graph normally opens already warm-started (settled), so this is inert
     * then; it only eases the rare residual motion (a node-drag reheat, or a
     * huge vault that hit the warm-start cap). Stops once the user takes manual
     * control (pan/zoom). */
    bool cam_moving = false;
    if (!fb->graph_user_zoomed) {
        f32 R = md_graph_world_radius(fb->graph);
        f32 target = fminf((f32)iw, (f32)ih) * 0.44f / fmaxf(R, 1.0f);
        f32 dz = target - fb->graph_zoom;
        bool unconverged = fabsf(dz) > target * 0.004f ||
                           fabsf(fb->graph_pan_x) > 0.5f || fabsf(fb->graph_pan_y) > 0.5f;
        if (moving || unconverged) {
            const f32 CAM_LERP = 0.12f;
            fb->graph_zoom  += dz * CAM_LERP;
            fb->graph_pan_x += (0.0f - fb->graph_pan_x) * CAM_LERP;
            fb->graph_pan_y += (0.0f - fb->graph_pan_y) * CAM_LERP;
            fb->graph_fit_zoom = fb->graph_zoom;   /* label-fade reference eases too */
            cam_moving = fabsf(target - fb->graph_zoom) > target * 0.004f ||
                         fabsf(fb->graph_pan_x) > 0.5f || fabsf(fb->graph_pan_y) > 0.5f;
        }
    }
    i32 hover = fb->graph_hover;

    /* Hover-focus animation. Easing graph_focus_t toward 1 while a node is
     * hovered (and back to 0 when not) lets the connected-web emphasis fade in
     * and out instead of snapping on/off. graph_focus_node centres the
     * highlight and outlives graph_hover through the fade-out, so the dimmed
     * neighbours keep their identity while the focus recedes. */
    if (hover >= 0) fb->graph_focus_node = hover;
    {
        f32 target = (hover >= 0) ? 1.0f : 0.0f;
        const f32 FOCUS_LERP = 0.22f;           /* matches the camera glide feel */
        f32 prev = fb->graph_focus_t;
        fb->graph_focus_t += (target - fb->graph_focus_t) * FOCUS_LERP;
        if (target > 0.5f && fb->graph_focus_t > 0.999f) fb->graph_focus_t = 1.0f;
        if (target < 0.5f && fb->graph_focus_t < 0.001f) fb->graph_focus_t = 0.0f;
        if (fb->graph_focus_t == 0.0f) fb->graph_focus_node = -1;
        if (fabsf(fb->graph_focus_t - prev) > 0.0001f)
            want_anim = true;   /* keep animating */
    }
    i32 focus_node = fb->graph_focus_node;
    f32 focus_t    = fb->graph_focus_t;

    /* Entrance fade (ease-out cubic). graph_intro_start is armed on first fit;
     * the graph content (edges/nodes/labels) is multiplied by intro_a so it
     * fades in as the camera glide eases the zoom up to the fit. */
    f32 intro_a = 1.0f;
    if (fb->graph_intro_start > 0.0) {
        f64 e = (platform_time_sec() - fb->graph_intro_start) / 0.45;
        if (e >= 1.0) { fb->graph_intro_start = 0.0; }
        else {
            f32 t = (f32)(e < 0.0 ? 0.0 : e);
            intro_a = 1.0f - (1.0f - t)*(1.0f - t)*(1.0f - t);
            want_anim = true;                               /* keep animating */
        }
    }

    /* Theme-derived palette, anchored to the chrome surface + a legible ink so
     * it reads on BOTH light and dark themes (no hardcoded RGB). */
    ChromePalette cp = chrome_palette_for(theme);
    Color accent  = theme_ui_accent(theme);
    Color fgc     = theme ? theme->fg : (Color){0.82f, 0.82f, 0.88f, 1};
    Color canvas  = cp.surface_sunken;                 /* tone the graph reads against */
    Color legible = chrome_legible_on(canvas);         /* black on light, white on dark */
    /* Node base: fg pulled 22% toward the legible direction so it always
     * separates from the canvas. */
    Color node = { fgc.r*0.78f + legible.r*0.22f,
                   fgc.g*0.78f + legible.g*0.22f,
                   fgc.b*0.78f + legible.b*0.22f, 1.0f };
    /* Edge: canvas→node blend, so links sit a clear tier below nodes on both
     * polarities (light-mid grey on white, dark-mid grey on black). */
    Color edge = { canvas.r + (node.r-canvas.r)*0.55f,
                   canvas.g + (node.g-canvas.g)*0.55f,
                   canvas.b + (node.b-canvas.b)*0.55f, 1.0f };
    /* Tag pseudo-nodes: a distinct green, legibility-corrected per polarity. */
    Color tag_raw = theme ? theme->ansi[2] : (Color){0.45f, 0.78f, 0.55f, 1};
    f32   tag_lum = chrome_luminance(tag_raw);
    Color tag_col = (cp.is_light && tag_lum > 0.55f) ? gcol_blend(tag_raw, node, 0.45f)
                  : (!cp.is_light && tag_lum < 0.10f) ? gcol_lift(tag_raw, 0.30f)
                  : tag_raw;

    /* Strong hover focus: build the hovered node's neighbour bitset ONCE
     * (O(edges)) so edges, arrows, nodes and labels all share O(1) lookups
     * rather than per-node md_graph_connected scans. */
    /* focusing stays true through the whole fade (focus_t > 0), driven by
     * focus_node rather than the live hover, so the highlight animates out
     * after the pointer leaves instead of cutting. */
    bool focusing = (focus_node >= 0 && focus_t > 0.0f);
    const f32 FOCUS_DIM = 0.06f;
    i32 nc = md_graph_node_count(fb->graph);
    bool have_nbr = false;
    if (focusing && nc > 0) {
        if (fb->graph_nbr_cap < nc) {
            u8 *nn = realloc(fb->graph_nbr_set, (usize)nc);
            if (nn) { fb->graph_nbr_set = nn; fb->graph_nbr_cap = nc; }
        }
        if (fb->graph_nbr_cap >= nc) {
            memset(fb->graph_nbr_set, 0, (usize)nc);
            i32 ne = md_graph_edge_count(fb->graph);
            for (i32 e = 0; e < ne; e++) {
                i32 a = -1, b = -1;
                md_graph_edge(fb->graph, e, &a, &b);
                if      (a == focus_node && b >= 0 && b < nc) fb->graph_nbr_set[b] = 1;
                else if (b == focus_node && a >= 0 && a < nc) fb->graph_nbr_set[a] = 1;
            }
            have_nbr = true;
        }
    }

    renderer_flush_rects(r);
    renderer_flush_glyphs(r);
    renderer_push_scissor(r, x, y, w, h);

    /* Faint canvas wash so nodes/edges read against the exact tone their
     * contrast was derived from (surface_sunken). Full-coverage, so it is
     * scaled by the window opacity — at alpha 1 it would be the largest
     * opaque surface in the app and blank out translucency/blur under the
     * whole graph pane. */
    { Color bgw = canvas; bgw.a = intro_a * opacity;
      renderer_draw_rect(r, x, y, w, h, bgw); renderer_flush_rects(r); }

    /* Edges — GPU anti-aliased lines (no CPU rasterization). */
    i32 ec = md_graph_edge_count(fb->graph);
    for (i32 e = 0; e < ec; e++) {
        i32 a = -1, b = -1;
        md_graph_edge(fb->graph, e, &a, &b);
        if (a < 0 || b < 0) continue;
        f32 ax, ay, ar, bx, by, br;
        md_graph_node_screen(fb->graph, a, iw, ih, fb->graph_pan_x,
                             fb->graph_pan_y, fb->graph_zoom, &ax, &ay, &ar);
        md_graph_node_screen(fb->graph, b, iw, ih, fb->graph_pan_x,
                             fb->graph_pan_y, fb->graph_zoom, &bx, &by, &br);
        if (graph_node_hidden(fb, a) || graph_node_hidden(fb, b)) continue;
        /* Edges incident to the focused node are the "connecting" edges — they
         * brighten toward accent while the rest melt to near-invisible, both
         * eased by focus_t so the transition glides in and out. */
        bool conn = (focusing && (a == focus_node || b == focus_node));
        Color ec_col = conn ? gcol_blend(edge, accent, 0.55f * focus_t) : edge;
        f32 ea_to = conn ? 0.90f : 0.05f;
        ec_col.a = (0.22f + (ea_to - 0.22f) * focus_t) * intro_a;
        f32 lt = fb->gset.link_thickness > 0 ? fb->gset.link_thickness : 1.0f;
        f32 ethick = (conn ? 0.85f + (1.6f - 0.85f) * focus_t : 0.85f) * dpi * lt;
        renderer_draw_line(r, x + ax, y + ay, x + bx, y + by, ethick, ec_col);
    }
    renderer_flush_lines(r);

    /* Arrowheads on directed links (Display ▸ Arrows). Drawn from the directed
     * src→dst edges so the head points the way the link actually goes; placed
     * just short of the destination node so it doesn't bury under the circle. */
    if (fb->gset.show_arrows) {
        i32 dec = md_graph_dir_edge_count(fb->graph);
        for (i32 e = 0; e < dec; e++) {
            i32 s = -1, d = -1;
            md_graph_dir_edge(fb->graph, e, &s, &d);
            if (s < 0 || d < 0) continue;
            if (graph_node_hidden(fb, s) || graph_node_hidden(fb, d)) continue;
            f32 sx, sy, sr, dx2, dy2, dr;
            md_graph_node_screen(fb->graph, s, iw, ih, fb->graph_pan_x,
                                 fb->graph_pan_y, fb->graph_zoom, &sx, &sy, &sr);
            md_graph_node_screen(fb->graph, d, iw, ih, fb->graph_pan_x,
                                 fb->graph_pan_y, fb->graph_zoom, &dx2, &dy2, &dr);
            f32 vx = dx2 - sx, vy = dy2 - sy;
            f32 len = sqrtf(vx*vx + vy*vy);
            if (len < dr + 6.0f) continue;
            f32 ux = vx / len, uy = vy / len;
            f32 tipx = dx2 - ux * (dr + 1.0f), tipy = dy2 - uy * (dr + 1.0f);
            f32 ah = 6.0f * dpi;                 /* arrowhead length */
            f32 px = -uy, py = ux;               /* perpendicular */
            bool conn = (focusing && (s == focus_node || d == focus_node));
            Color ac = conn ? gcol_blend(edge, accent, 0.55f * focus_t) : edge;
            f32 aa_to = conn ? 0.90f : 0.05f;
            ac.a = (0.40f + (aa_to - 0.40f) * focus_t) * intro_a;
            renderer_draw_line(r, x + tipx, y + tipy,
                               x + tipx - ux*ah + px*ah*0.55f,
                               y + tipy - uy*ah + py*ah*0.55f, 0.9f*dpi, ac);
            renderer_draw_line(r, x + tipx, y + tipy,
                               x + tipx - ux*ah - px*ah*0.55f,
                               y + tipy - uy*ah - py*ah*0.55f, 0.9f*dpi, ac);
        }
        renderer_flush_lines(r);
    }

    /* Nodes — GPU rounded circles (rrect with radius = size/2). */
    i32 gnc = md_graph_node_count(fb->graph);
    for (i32 i = 0; i < gnc; i++) {
        f32 sx, sy, rad;
        md_graph_node_screen(fb->graph, i, iw, ih, fb->graph_pan_x,
                             fb->graph_pan_y, fb->graph_zoom, &sx, &sy, &rad);
        if (sx < -rad || sx > w + rad || sy < -rad || sy > h + rad) continue;
        if (graph_node_hidden(fb, i)) continue;
        bool is_hover = (focusing && i == focus_node);
        bool is_neighbor = (focusing && i != focus_node && have_nbr && fb->graph_nbr_set[i]);
        bool is_tag = md_graph_node_is_tag(fb->graph, i);
        /* Default (color_mode 0): a single uniform node colour. Clusters mode (1):
         * hue per topic-component so a hub and its notes share a colour; isolated
         * notes stay neutral. Tags are always green so they read as a distinct kind. */
        Color c;
        if (is_tag) c = tag_col;
        else if (fb->gset.color_mode == 1 && md_graph_node_comp_size(fb->graph, i) >= 2)
            c = graph_group_color(md_graph_node_component(fb->graph, i), node, cp.is_light);
        else c = node;
        /* Focus mode: the focused node + its neighbours stay fully lit by ALPHA
         * (no fill flood to accent — the focused node's accent is the ring drawn
         * below); everything unrelated melts toward FOCUS_DIM. Each level is
         * eased by focus_t from the neutral 0.90 so the dim/brighten glides. */
        f32 rr = fmaxf(rad, 2.2f * dpi);     /* HiDPI visibility floor */
        f32 a;
        bool dimmed = !is_hover && !is_neighbor;
        if (is_hover)        { a = 0.90f + (1.0f - 0.90f) * focus_t; rr += 2.0f * dpi * focus_t; }
        else if (is_neighbor){ a = 0.90f + (1.0f - 0.90f) * focus_t; }
        else                 { a = 0.90f + (FOCUS_DIM - 0.90f) * focus_t; }
        c.a = a * intro_a;
        /* A handful of hubs (degree ≥ 8) get a faint SDF presence-glow; on a
         * dimmed (unrelated) node it fades out with focus_t rather than cutting. */
        f32 glow = (cp.is_light ? 0.08f : 0.12f) * intro_a;
        if (dimmed) glow *= (1.0f - focus_t);
        if (md_graph_node_degree(fb->graph, i) >= 8 && glow > 0.002f) {
            renderer_draw_rrect(r, x + sx - rr, y + sy - rr, rr * 2.0f, rr * 2.0f, c,
                                rr, rr, rr, rr,
                                rad * 0.9f, glow, 0, 0);
        } else {
            renderer_draw_rrect_simple(r, x + sx - rr, y + sy - rr,
                                       rr * 2.0f, rr * 2.0f, c, rr);
        }
    }
    renderer_flush_rrects(r);

    /* Hovered-node emphasis: an accent ring + soft halo (Metal SDF; OpenGL
     * degrades to just the ring). Drawn after the fill pass so it sits on top;
     * neighbours stay calm (lit by alpha alone, no ring). */
    if (focusing && focus_node >= 0 && !graph_node_hidden(fb, focus_node)) {
        f32 hsx, hsy, hrad;
        md_graph_node_screen(fb->graph, focus_node, iw, ih, fb->graph_pan_x,
                             fb->graph_pan_y, fb->graph_zoom, &hsx, &hsy, &hrad);
        bool h_tag = md_graph_node_is_tag(fb->graph, focus_node);
        Color hf = h_tag ? tag_col
                 : (fb->gset.color_mode == 1 && md_graph_node_comp_size(fb->graph, focus_node) >= 2)
                     ? graph_group_color(md_graph_node_component(fb->graph, focus_node), node, cp.is_light)
                     : node;
        hf.a = 1.0f * intro_a;
        /* Ring + halo strength tracks focus_t so the accent emphasis grows in
         * and recedes with the rest of the focus animation. */
        Color hb = accent; hb.a = 0.9f * intro_a * focus_t;
        f32 rr = fmaxf(hrad, 2.2f * dpi) + 2.0f * dpi * focus_t;
        renderer_draw_rrect_bordered(r, x + hsx - rr, y + hsy - rr, rr * 2.0f, rr * 2.0f,
                                     hf, hb, fmaxf(1.0f, 1.5f * dpi),
                                     rr, rr, rr, rr,
                                     8.0f * dpi, (cp.is_light ? 0.30f : 0.45f) * intro_a * focus_t, 0, 0);
        renderer_flush_rrects(r);
    }

    /* Node labels — DECLUTTERED. Drawing every label produced unreadable
     * overlapping mush, so labels are placed greedily by priority (hovered node
     * + its neighbours first, then hubs by degree) against a coarse screen-space
     * occupancy grid: a label is drawn only if its box doesn't collide with one
     * already placed. A degree gate + zoom fade reveal more labels at higher zoom;
     * hovering focuses to a node and its neighbours. */
    f32 lcw = 6.0f * dpi, lch = 12.0f * dpi;
    renderer_set_ui_scale(r, lcw, lch);
    /* Labels read as the node tone (inherits guaranteed canvas contrast on both
     * polarities); they go up to 0.98 alpha so they stay crisp over the nodes. */
    Color lbase = node;
    /* Fade is RELATIVE to the fitted zoom (the wider layout makes the absolute
     * fit zoom small, so an absolute gate would hide every label by default).
     * 0 at fit, 1 at ~2x fit. A legible floor keeps gated-in hubs readable. */
    f32 fitz = fb->graph_fit_zoom > 1e-4f ? fb->graph_fit_zoom : 1.0f;
    f32 zoom_t = (fb->graph_zoom / fitz) - 1.0f;
    if (zoom_t < 0.0f) zoom_t = 0.0f;
    if (zoom_t > 1.0f) zoom_t = 1.0f;
    f32 tf = fb->gset.text_fade > 0 ? fb->gset.text_fade : 0.0f;
    f32 base_alpha = (0.50f + 0.28f * zoom_t) * tf * intro_a;
    if (base_alpha > 1.0f) base_alpha = 1.0f;
    /* nc + the hovered-node neighbour bitset (have_nbr) were built once near the
     * top of the draw, shared by edges/arrows/nodes and reused here. */

    /* Occupancy grid (per-FileBrowser scratch, grown lazily, reused per frame). */
    LblGrid grid = { NULL, 0, 0, 26.0f * dpi, w, h };
    grid.cols = lbl_clampi((i32)(w / grid.cell) + 1, 1, 80);
    grid.rows = lbl_clampi((i32)(h / grid.cell) + 1, 1, 80);
    i32 ncell = grid.cols * grid.rows;
    if (fb->graph_lbl_grid_cap < ncell) {
        u8 *ng = realloc(fb->graph_lbl_grid, (usize)ncell);
        if (ng) { fb->graph_lbl_grid = ng; fb->graph_lbl_grid_cap = ncell; }
    }
    bool can_declutter = (fb->graph_lbl_grid_cap >= ncell);
    if (can_declutter) {
        grid.cells = fb->graph_lbl_grid;
        memset(grid.cells, 0, (usize)ncell);

        /* Reserve folder-cluster label boxes first so node labels yield to them
         * (the folder glyphs themselves are drawn in the block just below). */
        {
            f32 ccw = 7.0f * dpi, cch = 14.0f * dpi;
            i32 gcnt = md_graph_group_count(fb->graph);
            for (i32 gi = 0; gi < gcnt; gi++) {
                const char *gname = md_graph_group_name(fb->graph, gi);
                if (!gname || !gname[0]) continue;
                f32 gwx, gwy;
                if (!md_graph_group_centroid(fb->graph, gi, &gwx, &gwy)) continue;
                f32 gsx = w * 0.5f + fb->graph_pan_x + gwx * fb->graph_zoom;
                f32 gsy = h * 0.5f + fb->graph_pan_y + gwy * fb->graph_zoom;
                const char *bn = strrchr(gname, '/'); bn = bn ? bn + 1 : gname;
                i32 bl = (i32)strlen(bn); if (bl > 24) bl = 24;
                f32 gcells = (f32)utf8_len((const u8 *)bn, (usize)bl);
                f32 bx0 = gsx - gcells * ccw * 0.5f, by0 = gsy - 20.0f * dpi;
                lbl_box_mark(&grid, bx0, by0, bx0 + gcells * ccw, by0 + cch);
            }
        }

        /* Gather candidates as packed (priority<<20 | index) keys, then sort
         * descending so the highest-priority labels claim grid space first.
         * Small vaults skip the degree gate (no clutter to thin out). */
        i32 deg_gate = (nc <= 30) ? 0 : (i32)(2.0f * (1.0f - zoom_t) + 0.5f);
        if (fb->graph_lbl_cap < nc) {
            u64 *nk = realloc(fb->graph_lbl_key, (usize)nc * sizeof(u64));
            if (nk) { fb->graph_lbl_key = nk; fb->graph_lbl_cap = nc; }
        }
        i32 ncand = 0;
        if (fb->graph_lbl_cap >= nc) {
            for (i32 i = 0; i < nc; i++) {
                if (graph_node_hidden(fb, i)) continue;
                bool hov = (i == focus_node);
                bool nbr = (have_nbr && i != focus_node && fb->graph_nbr_set[i]);
                i32 deg = md_graph_node_degree(fb->graph, i);
                if (!hov && !nbr) {
                    if (base_alpha < 0.04f) continue;
                    if (deg < deg_gate) continue;
                }
                f32 sx, sy, rad;
                md_graph_node_screen(fb->graph, i, iw, ih, fb->graph_pan_x,
                                     fb->graph_pan_y, fb->graph_zoom, &sx, &sy, &rad);
                if (sx < -60 || sx > w + 60 || sy < -20 || sy > h + 20) continue;
                u32 special = hov ? 3u : (nbr ? 2u : 0u);
                u32 dd = (u32)(deg > 0xFFF ? 0xFFF : (deg < 0 ? 0 : deg));
                i32 csz = md_graph_node_comp_size(fb->graph, i);
                u32 cs = (u32)(csz > 0xFFF ? 0xFFF : (csz < 0 ? 0 : csz));
                u32 pri = (special << 28) | (dd << 12) | cs;
                fb->graph_lbl_key[ncand++] = ((u64)pri << 20) | (u32)i;
            }
            qsort(fb->graph_lbl_key, (usize)ncand, sizeof(u64), lbl_key_desc);
        }

        /* Greedy placement, capped so a pathological hover-fan can't flood. */
        i32 placed = 0;
        for (i32 k = 0; k < ncand && placed < 140; k++) {
            i32 i = (i32)(fb->graph_lbl_key[k] & 0xFFFFFu);
            bool hov = (i == focus_node);
            bool nbr = (have_nbr && i != focus_node && fb->graph_nbr_set[i]);
            /* Focus mode: focus_t lifts the focused node + its neighbours and
             * mutes everything else, eased so labels fade in step with the
             * nodes/edges instead of blinking on at hover. */
            f32 la;
            if (hov)      la = base_alpha + (0.98f - base_alpha) * focus_t;
            else if (nbr) la = base_alpha + (0.85f - base_alpha) * focus_t;
            else          la = base_alpha * (1.0f - focus_t);
            if (la < 0.04f) continue;
            f32 sx, sy, rad;
            md_graph_node_screen(fb->graph, i, iw, ih, fb->graph_pan_x,
                                 fb->graph_pan_y, fb->graph_zoom, &sx, &sy, &rad);
            const char *txt = md_graph_node_label(fb->graph, i);
            if (!txt || !txt[0]) continue;
            i32 blen = (i32)strlen(txt);
            if (blen > 28) blen = 28;
            f32 cells = (f32)utf8_len((const u8 *)txt, (usize)blen);
            if (cells < 1.0f) continue;
            f32 tw = cells * lcw, th = lch;
            f32 bx0 = sx - tw * 0.5f, by0 = sy + rad + 5.0f * dpi;
            f32 bx1 = bx0 + tw, by1 = by0 + th;
            bool force = hov || nbr;     /* always show; still reserve its box */
            if (!force && !lbl_box_free(&grid, bx0, by0, bx1, by1)) continue;
            lbl_box_mark(&grid, bx0, by0, bx1, by1);
            placed++;
            f32 gx = x + bx0, gy = y + by0;
            Color label = lbase; label.a = la;
            const u8 *p = (const u8 *)txt; usize rem = (usize)blen;
            while (rem > 0) {
                u32 cp = 0;
                u32 nn = utf8_decode(p, rem, &cp);
                if (nn == 0) break;
                if (cp >= 32) renderer_push_glyph(r, gx, gy, cp, label);
                gx += lcw; p += nn; rem -= nn;
            }
        }
    }
    renderer_flush_glyphs(r);

    /* Folder cluster labels — the directory name at each cluster's centroid, in
     * the cluster's colour, so the groups read at a glance. Drawn at their own
     * (slightly larger) glyph scale after the node labels are flushed. */
    {
        f32 ccw = 7.0f * dpi, cch = 14.0f * dpi;
        renderer_set_ui_scale(r, ccw, cch);
        i32 gcnt = md_graph_group_count(fb->graph);
        for (i32 gi = 0; gi < gcnt; gi++) {
            const char *gname = md_graph_group_name(fb->graph, gi);
            if (!gname || !gname[0]) continue;           /* root files: no label */
            f32 gwx, gwy;
            if (!md_graph_group_centroid(fb->graph, gi, &gwx, &gwy)) continue;
            f32 gsx = (f32)w * 0.5f + fb->graph_pan_x + gwx * fb->graph_zoom;
            f32 gsy = (f32)h * 0.5f + fb->graph_pan_y + gwy * fb->graph_zoom;
            if (gsx < -80 || gsx > w + 80 || gsy < -40 || gsy > h + 40) continue;
            const char *bn = strrchr(gname, '/'); bn = bn ? bn + 1 : gname;
            i32 blen = (i32)strlen(bn); if (blen > 24) blen = 24;
            f32 cells = (f32)utf8_len((const u8 *)bn, (usize)blen);
            f32 gx = x + gsx - cells * ccw * 0.5f;
            f32 gy = y + gsy - 20.0f * dpi;
            /* Folder labels are neutral: node colour conveys topic clusters
             * (components), not folders, so a group-coloured label would imply
             * a node-colour match that does not hold. */
            Color lc = node; lc.a = 0.50f * intro_a;
            const u8 *p = (const u8 *)bn; usize rem = (usize)blen;
            while (rem > 0) {
                u32 cp = 0; u32 nb = utf8_decode(p, rem, &cp);
                if (nb == 0) break;
                if (cp >= 32) renderer_push_glyph(r, gx, gy, cp, lc);
                gx += ccw; p += nb; rem -= nb;
            }
        }
        renderer_flush_glyphs(r);
    }

    renderer_pop_scissor(r);
    renderer_reset_ui_scale(r);

    /* Controls panel floats above the (unclipped) canvas. */
    graph_render_panel(fb, r, dpi, theme);

    /* Keep redrawing while the sim is moving, the camera is still gliding to
     * its fit, OR any fade (hover focus / intro) asked for another frame —
     * this is the ONLY writer at frame end, so no animation request gets
     * stomped. */
    if (moving || cam_moving || want_anim)
        fb->graph_next_frame_at = platform_time_sec();
    else
        fb->graph_next_frame_at = 0.0;
}

/* ---- Graph interaction (rel coords within the viewer content rect) ---- */

void fb_graph_hover(FileBrowser *fb, f32 rx, f32 ry, f32 w, f32 h) {
    if (!fb_graph_active(fb)) return;
    if (fb->graph_panning || fb->graph_drag_node >= 0) return;
    fb->graph_hover = graph_hit_visible(fb, rx, ry, w, h);
}

void fb_graph_press(FileBrowser *fb, f32 rx, f32 ry, f32 w, f32 h) {
    if (!fb_graph_active(fb)) return;
    fb->graph_press_x = fb->graph_last_x = rx;
    fb->graph_press_y = fb->graph_last_y = ry;
    fb->graph_moved = false;
    i32 node = graph_hit_visible(fb, rx, ry, w, h);
    if (node >= 0) { fb->graph_drag_node = node; fb->graph_panning = false; }
    else           { fb->graph_panning = true;   fb->graph_drag_node = -1; }
}

void fb_graph_move(FileBrowser *fb, f32 rx, f32 ry, f32 w, f32 h) {
    if (!fb_graph_active(fb)) return;
    f32 ddx = rx - fb->graph_last_x, ddy = ry - fb->graph_last_y;
    fb->graph_last_x = rx; fb->graph_last_y = ry;
    if (fabsf(rx - fb->graph_press_x) > 3.0f || fabsf(ry - fb->graph_press_y) > 3.0f)
        fb->graph_moved = true;
    if (fb->graph_drag_node >= 0) {
        f32 wx, wy;
        md_graph_screen_to_world((i32)w, (i32)h, fb->graph_pan_x, fb->graph_pan_y,
                                 fb->graph_zoom, rx, ry, &wx, &wy);
        md_graph_set_node_world(fb->graph, fb->graph_drag_node, wx, wy, true);
        md_graph_reheat(fb->graph);
        /* The reheat restarts motion; without this the auto re-fit would stomp
         * the user's pan/zoom back to the fitted view every frame of the drag. */
        fb->graph_user_zoomed = true;
        fb->graph_next_frame_at = platform_time_sec();
    } else if (fb->graph_panning) {
        fb->graph_pan_x += ddx; fb->graph_pan_y += ddy;
        fb->graph_user_zoomed = true;
    }
}

const char *fb_graph_release(FileBrowser *fb, f32 rx, f32 ry, f32 w, f32 h) {
    if (!fb_graph_active(fb)) return NULL;
    const char *open_path = NULL;
    if (!fb->graph_moved) {
        i32 node = graph_hit_visible(fb, rx, ry, w, h);
        if (node >= 0) open_path = md_graph_node_path(fb->graph, node);
    }
    /* Unpin a dragged node so the layout can keep relaxing it. */
    if (fb->graph_drag_node >= 0) {
        f32 wx, wy;
        md_graph_screen_to_world((i32)w, (i32)h, fb->graph_pan_x, fb->graph_pan_y,
                                 fb->graph_zoom, rx, ry, &wx, &wy);
        md_graph_set_node_world(fb->graph, fb->graph_drag_node, wx, wy, false);
    }
    fb->graph_drag_node = -1;
    fb->graph_panning = false;
    return open_path;
}

void fb_graph_scroll(FileBrowser *fb, f32 dy, bool precise,
                     f32 rx, f32 ry, f32 w, f32 h) {
    if (!fb_graph_active(fb)) return;
    /* Zoom toward the cursor: keep the world point under (rx,ry) fixed.
     * Sensitivity is split by input device so neither overshoots: a trackpad
     * sends pixel-precise dy (many small events per gesture), a wheel sends a
     * few discrete lines — one wheel line is a controlled ~5.5% step, a
     * trackpad gesture eases in gently. */
    f32 wx, wy;
    md_graph_screen_to_world((i32)w, (i32)h, fb->graph_pan_x, fb->graph_pan_y,
                             fb->graph_zoom, rx, ry, &wx, &wy);
    f32 zoom_in = precise ? dy * 0.0020f    /* per trackpad pixel */
                          : dy * 0.055f;    /* per wheel line     */
    f32 factor = expf(zoom_in);
    f32 nz = fb->graph_zoom * factor;
    if (nz < 0.05f) nz = 0.05f; if (nz > 12.0f) nz = 12.0f;
    fb->graph_zoom = nz;
    fb->graph_user_zoomed = true;   /* user took control: stop auto re-fit */
    /* Recompute pan so (wx,wy) stays under the cursor. */
    fb->graph_pan_x = rx - (f32)w * 0.5f - wx * nz;
    fb->graph_pan_y = ry - (f32)h * 0.5f - wy * nz;
    fb->graph_next_frame_at = platform_time_sec();  /* repaint at the new view */
}

bool fb_graph_gesture(const FileBrowser *fb) {
    return fb && fb->view_mode == FVIEW_GRAPH &&
           (fb->graph_panning || fb->graph_drag_node >= 0 ||
            fb->graph_drag_slider >= 0);
}

/* ---- Graph controls panel input (absolute framebuffer coords) ---------- */

bool fb_graph_panel_hit(const FileBrowser *fb, f32 sx, f32 sy) {
    if (!fb_graph_active(fb)) return false;
    FileBrowser *m = (FileBrowser *)fb;   /* layout only reads geometry/settings */
    GCtl c[GRAPH_PANEL_CTLS];
    f32 gx,gy,gs,px,py,pw,ph,cx,cw;
    f32 dpi = fb->graph_render_dpi > 0 ? fb->graph_render_dpi : 1.0f;
    graph_panel_layout(m, dpi, c, &gx,&gy,&gs, &px,&py,&pw,&ph, &cx,&cw);
    if (sx >= gx && sx < gx+gs && sy >= gy && sy < gy+gs) return true;
    if (fb->graph_settings_open && sx >= px && sx < px+pw && sy >= py && sy < py+ph)
        return true;
    return false;
}

bool fb_graph_panel_press(FileBrowser *fb, f32 sx, f32 sy) {
    if (!fb_graph_active(fb)) return false;
    GCtl c[GRAPH_PANEL_CTLS];
    f32 gx,gy,gs,px,py,pw,ph,cx,cw;
    f32 dpi = fb->graph_render_dpi > 0 ? fb->graph_render_dpi : 1.0f;
    graph_panel_layout(fb, dpi, c, &gx,&gy,&gs, &px,&py,&pw,&ph, &cx,&cw);

    if (sx >= gx && sx < gx+gs && sy >= gy && sy < gy+gs) {
        fb->graph_settings_open = !fb->graph_settings_open;
        return true;
    }
    if (!fb->graph_settings_open) return false;
    if (!(sx >= px && sx < px+pw && sy >= py && sy < py+ph)) return false;

    for (i32 i = 0; i < GRAPH_PANEL_CTLS; i++) {
        if (sy < c[i].ry || sy >= c[i].ry + c[i].rh) continue;
        if (c[i].kind == GC_SLIDER) {
            f32 tx,ty,tw,th; gc_track(&c[i], cx, cw, dpi, &tx,&ty,&tw,&th);
            f32 t = (sx - tx) / tw; if (t < 0) t = 0; if (t > 1) t = 1;
            *c[i].fval = c[i].minv + t * (c[i].maxv - c[i].minv);
            fb->graph_drag_slider = i;
            fb->gset_dirty = true;
            if (c[i].is_force && fb->graph) md_graph_reheat(fb->graph);
        } else {
            if (c[i].ival)      *c[i].ival = *c[i].ival ? 0 : 1;
            else if (c[i].bval) *c[i].bval = !*c[i].bval;
            fb->gset_dirty = true;
        }
        return true;
    }
    return true;   /* swallow clicks anywhere inside the panel */
}

void fb_graph_panel_drag(FileBrowser *fb, f32 sx, f32 sy) {
    (void)sy;
    if (!fb || fb->graph_drag_slider < 0) return;
    GCtl c[GRAPH_PANEL_CTLS];
    f32 gx,gy,gs,px,py,pw,ph,cx,cw;
    f32 dpi = fb->graph_render_dpi > 0 ? fb->graph_render_dpi : 1.0f;
    graph_panel_layout(fb, dpi, c, &gx,&gy,&gs, &px,&py,&pw,&ph, &cx,&cw);
    i32 i = fb->graph_drag_slider;
    if (i < 0 || i >= GRAPH_PANEL_CTLS || c[i].kind != GC_SLIDER) return;
    f32 tx,ty,tw,th; gc_track(&c[i], cx, cw, dpi, &tx,&ty,&tw,&th);
    f32 t = (sx - tx) / tw; if (t < 0) t = 0; if (t > 1) t = 1;
    f32 nv = c[i].minv + t * (c[i].maxv - c[i].minv);
    if (nv == *c[i].fval) return;          /* no-op move: don't re-arm the O(n^2) sim */
    *c[i].fval = nv;
    fb->gset_dirty = true;
    if (c[i].is_force && fb->graph) md_graph_reheat(fb->graph);
}

void fb_graph_panel_release(FileBrowser *fb) {
    if (fb) fb->graph_drag_slider = -1;
}

static bool fb_open_path_mode(FileBrowser *fb, const char *abs_path, bool edit, bool keep) {
    if (!fb || !abs_path || !abs_path[0] || strlen(abs_path) >= FB_MAX_PATH) return false;
    char dir[FB_MAX_PATH];
    snprintf(dir, sizeof dir, "%s", abs_path);
    char *slash = strrchr(dir, '/');
    char base[256];
    snprintf(base, sizeof base, "%s", slash ? slash + 1 : dir);
    char normalized_base[sizeof base];
    if (platform_utf8_normalize_nfc(base, normalized_base, sizeof normalized_base))
        snprintf(base, sizeof base, "%s", normalized_base);
    if (slash == dir) slash[1] = '\0';
    else if (slash) *slash = '\0';
    else snprintf(dir, sizeof dir, ".");
    /* The document that is already open, with edits in it, is kept as it is;
     * any other one with edits is not closed under the first request. */
    if (fb_editor_dirty(fb) && !strcmp(fb->view_path, abs_path)) {
        fb->kbd_away = false;
        fb_open_set_edit(fb, edit);
        return true;
    }
    /* When a local target is missing, show a not-found message and leave the
     * open document unchanged. */
    if (fb->source == FB_SOURCE_LOCAL) {
        struct stat tst;
        if (stat(abs_path, &tst) != 0) {
            fb_notice(fb, true, "Not found: %s", base);
            return false;
        }
    }
    if (!fb_discard_ok(fb, UNSAVED_OPEN)) return false;
    fb_close_viewer_now(fb, keep);       /* leave the old doc, keep the graph */
    char display_dir[sizeof dir];
    if (!platform_utf8_normalize_nfc(dir, display_dir, sizeof display_dir))
        snprintf(display_dir, sizeof display_dir, "%s", dir);
    bool listing_same = fb->source == FB_SOURCE_LOCAL && fb->local_list_job &&
                        !fb->local_list_job->tree && !strcmp(fb->cwd, display_dir);
    if (!listing_same && !fb_navigate(fb, dir)) {
        if (keep) fb->graph_return = true;
        return false;
    }
    if (fb->local_list_job) {
        /* Slow folder: open the note when its listing lands. */
        snprintf(fb->pending_open_name, sizeof fb->pending_open_name, "%s", base);
        fb->pending_open_keep_graph = keep;
        fb->pending_open_edit = edit;
        return true;
    }
    for (i32 i = 0; i < fb->entry_count; i++) {
        if (fb->entries[i].name && strcmp(fb->entries[i].name, base) == 0) {
            fb->selected = i;
            bool ok = fb_open_file_edit(fb, i, edit);
            if (keep && fb->graph && fb->view_mode != FVIEW_GRAPH)
                fb->graph_return = true;
            return ok;
        }
    }
    if (keep) fb->graph_return = true;   /* not found: Esc can still return */
    return false;
}

bool fb_open_path(FileBrowser *fb, const char *path, bool edit) {
    return fb_open_path_mode(fb, path, edit, false);
}

bool fb_open_md_path(FileBrowser *fb, const char *path) {
    /* A wikilink/embed hop preserves the graph return context. */
    bool keep = fb && fb->graph_return && fb->graph;
    return fb_open_path_mode(fb, path, false, keep);
}

/* Shared source-to-screen walk for wrap, caret following and hit testing.
 * Layout never changes the buffer or inserts newlines into copied text. */
static f32 fb_live_layout(FileBrowser *fb, bool locate, f32 mx, f32 target_y,
                          f32 *caret_y, f32 *caret_h) {
    const char *p = fb->view_content, *end = p + fb->view_size;
    MdLiveFence fs = {0};
    f32 y = 0.0f;
    i32 line = 0;
    bool found = false, top_found = false;
    for (;;) {
        const char *nl = memchr(p, '\n', (usize)(end - p));
        i32 len = nl ? (i32)(nl - p) : (i32)(end - p);
        if (nl && len && p[len - 1] == '\r') len--;
        MdLiveLine cls = md_live_classify(p, len, &fs);
        i32 cols = md_live_wrap_cols(cls, fb->live_width, fb->live_cw);
        f32 cw = md_live_cell_w(cls.heading, fb->live_cw);
        f32 lh = md_live_line_h(cls.heading, fb->live_ch, fb->live_dpi);
        f32 line_y = y;
        i32 at = 0;
        do {
            i32 stop = md_live_wrap_end(p, len, at, cols);
            if (!locate && !top_found && y + lh > fb->view_scroll_px) {
                fb->view_scroll = line;
                fb->live_skip_px = fb->view_scroll_px - line_y;
                top_found = true;
            }
            if (caret_y && line == fb->cursor_line && fb->cursor_col >= at &&
                (fb->cursor_col < stop || stop == len)) {
                *caret_y = y; *caret_h = lh;
                i32 column = editor_col_at(p + at, stop - at, fb->cursor_col - at, 1);
                if (cls.code || cls.fence) column -= fb->view_scroll_x;
                fb->live_cursor_x = fb->md_origin_x + column * cw;
            }
            if (locate && !found && target_y < y + lh) {
                f32 col = (mx - fb->md_origin_x) / cw;
                if (cls.code || cls.fence) col += fb->view_scroll_x;
                i32 byte = editor_byte_at_point(p + at, stop - at, col, 1);
                /* A click at a soft row's right edge belongs to that row. */
                if (at + byte == stop && stop < len && byte > 0)
                    byte -= editor_char_back(p + at, stop - at, byte);
                fb->cursor_line = line; fb->cursor_col = at + byte;
                found = true;
            }
            y += lh; at = stop;
        } while (at < len);
        if (!nl) break;
        p = nl + 1; line++;
    }
    if (locate && !found) { fb->cursor_line = line; fb->cursor_col = (i32)(end - p); }
    return y;
}

static void fb_live_prepare(FileBrowser *fb, f32 height) {
    if (fb->live_layout_cache.valid && !fb->kbd_away &&
        (fb->live_layout_cache.width != fb->live_width || fb->live_layout_cache.height != height))
        fb->live_follow = true;
    if (!fb->live_follow && fb->live_layout_cache.valid &&
        fb->live_layout_cache.buffer == fb->view_content &&
        fb->live_layout_cache.generation == fb->view_edit_gen &&
        fb->live_layout_cache.line == fb->cursor_line && fb->live_layout_cache.col == fb->cursor_col &&
        fb->live_layout_cache.width == fb->live_width && fb->live_layout_cache.height == height &&
        fb->live_layout_cache.cw == fb->live_cw && fb->live_layout_cache.ch == fb->live_ch &&
        fb->live_layout_cache.dpi == fb->live_dpi && fb->live_layout_cache.scroll == fb->view_scroll_px)
        return;
    f32 old_scroll = fb->view_scroll_px;
    f32 cy = 0.0f, lh = fb->live_ch;
    f32 total = fb_live_layout(fb, false, 0, 0, &cy, &lh);
    if (fb->live_follow) {
        if (cy < fb->view_scroll_px) fb->view_scroll_px = cy;
        if (cy + lh > fb->view_scroll_px + height) fb->view_scroll_px = cy + lh - height;
        fb->live_follow = false;
    }
    fb->view_scroll_px = fmaxf(0.0f, fminf(fb->view_scroll_px, total - height));
    if (old_scroll != fb->view_scroll_px) fb_live_layout(fb, false, 0, 0, NULL, NULL);
    fb->view_content_px = total;
    fb->live_layout_cache.buffer = fb->view_content;
    fb->live_layout_cache.generation = fb->view_edit_gen;
    fb->live_layout_cache.line = fb->cursor_line; fb->live_layout_cache.col = fb->cursor_col;
    fb->live_layout_cache.width = fb->live_width; fb->live_layout_cache.height = height;
    fb->live_layout_cache.cw = fb->live_cw; fb->live_layout_cache.ch = fb->live_ch;
    fb->live_layout_cache.dpi = fb->live_dpi; fb->live_layout_cache.scroll = fb->view_scroll_px;
    fb->live_layout_cache.valid = true;
}

/* Draw a short inactive source line with hidden Markdown markers. The caller
 * sets the glyph scale; each visible glyph advances by cw. */
static f32 fb_live_inline(Renderer *r, const Theme *th, const char *s, i32 len,
                          f32 x, f32 y, f32 cw, f32 ch, Color fg) {
    Color bright = { fminf(1.0f,fg.r+0.18f), fminf(1.0f,fg.g+0.18f), fminf(1.0f,fg.b+0.18f), fg.a };
    Color dim    = { fg.r*0.60f, fg.g*0.60f, fg.b*0.60f, fg.a };
    Color link   = th ? th->ansi[12] : (Color){0.45f,0.62f,1.0f,1.0f};
    Color tagc   = th ? th->ansi[2]  : (Color){0.45f,0.78f,0.55f,1.0f};
    Color codefg = th ? th->ansi[11] : (Color){0.92f,0.80f,0.52f,1.0f};
    Color codebg = (Color){1,1,1,0.07f};
    Color hlbg   = (Color){0.95f,0.85f,0.20f,0.22f};
    i32 i = 0;
#define LIV_RUN(from,to,col) do { for (i32 _k=(from); _k<(to);) { u32 _cp; \
        u32 _nb=utf8_decode((const u8*)s+_k,(usize)(len-_k),&_cp); if(!_nb){_k++;continue;} \
        if(_cp>=32){ renderer_push_glyph(r,x,y,_cp,(col)); x+=cw; } _k+=_nb; } } while(0)
    while (i < len) {
        unsigned char c = (unsigned char)s[i];
        if (c=='*' && i+1<len && s[i+1]=='*') {                 /* **bold** */
            i32 j=i+2; while(j+1<len && !(s[j]=='*'&&s[j+1]=='*')) j++;
            if (j+1<len) { LIV_RUN(i+2,j,bright); i=j+2; continue; }
        }
        if (c=='*' || c=='_') {                                 /* *italic* _italic_ */
            i32 j=i+1; while(j<len && s[j]!=(char)c) j++;
            if (j<len && j>i+1) { LIV_RUN(i+1,j,dim); i=j+1; continue; }
        }
        if (c=='`') {                                           /* `code` */
            i32 j=i+1; while(j<len && s[j]!='`') j++;
            if (j<len) {
                f32 span=(f32)utf8_len((const u8*)s+i+1,(usize)(j-i-1))*cw;
                renderer_draw_rect(r, x-1.0f, y-1.0f, span+2.0f, ch+2.0f, codebg);
                LIV_RUN(i+1,j,codefg); i=j+1; continue;
            }
        }
        if (c=='~' && i+1<len && s[i+1]=='~') {                 /* ~~strike~~ */
            i32 j=i+2; while(j+1<len && !(s[j]=='~'&&s[j+1]=='~')) j++;
            if (j+1<len) { LIV_RUN(i+2,j,dim); i=j+2; continue; }
        }
        if (c=='=' && i+1<len && s[i+1]=='=') {                 /* ==highlight== */
            i32 j=i+2; while(j+1<len && !(s[j]=='='&&s[j+1]=='=')) j++;
            if (j+1<len) {
                f32 span=(f32)utf8_len((const u8*)s+i+2,(usize)(j-i-2))*cw;
                renderer_draw_rect(r, x-1.0f, y, span+2.0f, ch, hlbg);
                LIV_RUN(i+2,j,fg); i=j+2; continue;
            }
        }
        if (c=='[' && i+1<len && s[i+1]=='[') {                 /* [[wiki|alias]] */
            i32 j=i+2; while(j+1<len && !(s[j]==']'&&s[j+1]==']')) j++;
            if (j+1<len) {
                i32 ds=i+2, de=j;
                for (i32 b=i+2;b<j;b++) if (s[b]=='|') { ds=b+1; break; }
                LIV_RUN(ds,de,link); i=j+2; continue;
            }
        }
        if (c=='[') {                                           /* [text](url) */
            i32 j=i+1; while(j<len && s[j]!=']') j++;
            if (j<len && j+1<len && s[j+1]=='(') {
                i32 p=j+2; while(p<len && s[p]!=')') p++;
                if (p<len) { LIV_RUN(i+1,j,link); i=p+1; continue; }
            }
        }
        if (c=='#' && (i==0 || s[i-1]==' ' || s[i-1]=='\t') && i+1<len &&
            (isalnum((unsigned char)s[i+1])||s[i+1]=='_'||s[i+1]=='/')) {   /* #tag */
            i32 j=i+1; while(j<len && (isalnum((unsigned char)s[j])||s[j]=='_'||s[j]=='-'||s[j]=='/')) j++;
            LIV_RUN(i,j,tagc); i=j; continue;
        }
        { u32 cp; u32 nb=utf8_decode((const u8*)s+i,(usize)(len-i),&cp);
          if(!nb){i++;continue;} if(cp>=32){ renderer_push_glyph(r,x,y,cp,fg); x+=cw; } i+=nb; }
    }
#undef LIV_RUN
    return x;
}

/* A line of a fenced block, or a fence itself: the bytes, and nothing else.
 * fb_live_inline and fb_live_inline_raw read `_x_`, `*x*`,
 * `` `x` ``, `[x](y)` and `#tag` as Markdown, which is right for prose and
 * wrong for code — in `wp_http_validate_url(` the two underscores would be an
 * emphasis pair, hidden, with `http` dimmed between them, and a fence line
 * `` ```php `` an empty inline-code span followed by a backtick. One cell per
 * character, as the caret on the active line counts them (a tab is one cell
 * here), the first `dim_len` bytes in `dim` (a fence's marks) and the rest in
 * `base`. */
static void fb_live_literal(Renderer *r, const char *s, i32 len, i32 dim_len,
                            f32 x, f32 y, f32 cw, Color base, Color dim) {
    for (i32 k = 0; k < len; ) {
        EditorCell c = editor_cell_at(s, len, k, 0, 1);
        if (c.cp) renderer_push_glyph(r, x, y, c.cp, k < dim_len ? dim : base);
        x += cw;
        k += c.bytes;
    }
}

/* Active-line variant of fb_live_inline: the cursor's source line is shown RAW
 * (every marker stays visible so editing is literal) but the markers are dimmed
 * and the wrapped text is tinted, so the raw line is readable instead of flat.
 * Every codepoint advances exactly one cell — matching the caret's per-codepoint
 * column count in the caller — so the caret stays glued to the right glyph.
 * `marker_len` is the leading block marker (heading #, quote >, list/-[ ] …). */
static void fb_live_inline_raw(Renderer *r, const Theme *th, const char *s, i32 len,
                               i32 marker_len, f32 x, f32 y, f32 cw, f32 ch, Color base) {
    Color bright = { fminf(1.0f,base.r+0.18f), fminf(1.0f,base.g+0.18f), fminf(1.0f,base.b+0.18f), base.a };
    Color dim    = { base.r*0.50f, base.g*0.50f, base.b*0.50f, base.a };
    Color link   = th ? th->ansi[12] : (Color){0.45f,0.62f,1.0f,1.0f};
    Color tagc   = th ? th->ansi[2]  : (Color){0.45f,0.78f,0.55f,1.0f};
    Color codefg = th ? th->ansi[11] : (Color){0.92f,0.80f,0.52f,1.0f};
    Color codebg = (Color){1,1,1,0.07f};
    Color hlbg   = (Color){0.95f,0.85f,0.20f,0.22f};
    f32 gx = x;
    /* Emit [from,to): one cell per character, a control character and a byte
     * that is not UTF-8 included — the caret on this line counts one column
     * for each, and a byte the painter skipped would put every glyph after
     * it one cell left of where the caret says it is. (Tab width 1: in this
     * view a tab is one cell, as its caret counts it.) */
#define RAW_RUN(from,to,col) do { for (i32 _k=(from); _k<(to);) { \
        EditorCell _c=editor_cell_at(s,len,_k,0,1); \
        if(_c.cp) renderer_push_glyph(r,gx,y,_c.cp,(col)); gx+=cw; _k+=_c.bytes; } } while(0)
    i32 i = 0;
    if (marker_len > 0) { i32 m = marker_len < len ? marker_len : len; RAW_RUN(0,m,dim); i = m; }
    while (i < len) {
        unsigned char c = (unsigned char)s[i];
        if (c=='*' && i+1<len && s[i+1]=='*') {                 /* **bold** */
            i32 j=i+2; while(j+1<len && !(s[j]=='*'&&s[j+1]=='*')) j++;
            if (j+1<len) { RAW_RUN(i,i+2,dim); RAW_RUN(i+2,j,bright); RAW_RUN(j,j+2,dim); i=j+2; continue; }
        }
        if (c=='*' || c=='_') {                                 /* *italic* _italic_ */
            i32 j=i+1; while(j<len && s[j]!=(char)c) j++;
            if (j<len && j>i+1) { RAW_RUN(i,i+1,dim); RAW_RUN(i+1,j,base); RAW_RUN(j,j+1,dim); i=j+1; continue; }
        }
        if (c=='`') {                                           /* `code` */
            i32 j=i+1; while(j<len && s[j]!='`') j++;
            if (j<len) {
                f32 span=(f32)utf8_len((const u8*)s+i,(usize)(j+1-i))*cw;
                renderer_draw_rect(r, gx-1.0f, y-1.0f, span+2.0f, ch+2.0f, codebg);
                RAW_RUN(i,i+1,dim); RAW_RUN(i+1,j,codefg); RAW_RUN(j,j+1,dim); i=j+1; continue;
            }
        }
        if (c=='~' && i+1<len && s[i+1]=='~') {                 /* ~~strike~~ */
            i32 j=i+2; while(j+1<len && !(s[j]=='~'&&s[j+1]=='~')) j++;
            if (j+1<len) { RAW_RUN(i,i+2,dim); RAW_RUN(i+2,j,dim); RAW_RUN(j,j+2,dim); i=j+2; continue; }
        }
        if (c=='=' && i+1<len && s[i+1]=='=') {                 /* ==highlight== */
            i32 j=i+2; while(j+1<len && !(s[j]=='='&&s[j+1]=='=')) j++;
            if (j+1<len) {
                f32 span=(f32)utf8_len((const u8*)s+i,(usize)(j+2-i))*cw;
                renderer_draw_rect(r, gx-1.0f, y, span+2.0f, ch, hlbg);
                RAW_RUN(i,i+2,dim); RAW_RUN(i+2,j,base); RAW_RUN(j,j+2,dim); i=j+2; continue;
            }
        }
        if (c=='[' && i+1<len && s[i+1]=='[') {                 /* [[wiki|alias]] */
            i32 j=i+2; while(j+1<len && !(s[j]==']'&&s[j+1]==']')) j++;
            if (j+1<len) { RAW_RUN(i,i+2,dim); RAW_RUN(i+2,j,link); RAW_RUN(j,j+2,dim); i=j+2; continue; }
        }
        if (c=='[') {                                           /* [text](url) */
            i32 j=i+1; while(j<len && s[j]!=']') j++;
            if (j<len && j+1<len && s[j+1]=='(') {
                i32 q=j+2; while(q<len && s[q]!=')') q++;
                if (q<len) {
                    RAW_RUN(i,i+1,dim); RAW_RUN(i+1,j,base);     /* [text] */
                    RAW_RUN(j,j+2,dim); RAW_RUN(j+2,q,dim); RAW_RUN(q,q+1,dim); /* ](url) */
                    i=q+1; continue;
                }
            }
        }
        if (c=='#' && (i==0 || s[i-1]==' ' || s[i-1]=='\t') && i+1<len &&
            (isalnum((unsigned char)s[i+1])||s[i+1]=='_'||s[i+1]=='/')) {   /* #tag */
            i32 j=i+1; while(j<len && (isalnum((unsigned char)s[j])||s[j]=='_'||s[j]=='-'||s[j]=='/')) j++;
            RAW_RUN(i,j,tagc); i=j; continue;
        }
        { EditorCell cell = editor_cell_at(s, len, i, 0, 1);
          if (cell.cp) renderer_push_glyph(r,gx,y,cell.cp,base); gx+=cw; i+=cell.bytes; }
    }
#undef RAW_RUN
}

static void fb_md_copy_finish(FileBrowser *fb, const MdGlyphRect *all, u32 all_count,
                              f32 body_y, f32 body_h);
void fb_render_viewer(FileBrowser *fb, Renderer *r, f32 x, f32 y, f32 w, f32 h,
                      f32 dpi, const Theme *theme, f32 opacity,
                      f32 hover_x, f32 hover_y) {
    fb_poll_async(fb);
    fb->caret_drawn = false;
    /* The graph short-circuits BEFORE the opacity clamp below, so it has to be
     * handed the caller's value directly. */
    if (fb->view_mode == FVIEW_GRAPH) {
        fb_render_graph(fb, r, x, y, w, h, dpi, theme, opacity);
        return;
    }
    if (fb->view_mode == FVIEW_NONE || !fb->view_content) return;

    /* Fixed UI scale for viewer */
    f32 cw = 8.0f * dpi;
    f32 ch = 16.0f * dpi;
    g_ui_step = cw;
    renderer_set_ui_scale(r, cw, ch);
    f32 pad = 12 * dpi;

    /* Theme-derived colors for the editor. The viewer is part of the app
     * chrome (lives next to sidebar/toolbar), so its panel/title/code
     * backgrounds fade together with the window opacity — otherwise the
     * viewer stays fully opaque over a translucent terminal body. */
    if (opacity < 0.0f) opacity = 0.0f;
    if (opacity > 1.0f) opacity = 1.0f;
    Color bg_color     = theme ? theme->bg      : (Color){0.10f, 0.10f, 0.12f, 1.0f};
    Color fg_color     = theme ? theme->fg      : (Color){0.82f, 0.82f, 0.85f, 1.0f};
    Color border_color = theme ? theme->border  : (Color){0.18f, 0.18f, 0.20f, 1.0f};
    Color sel_color    = theme ? theme->selection: (Color){0.20f, 0.30f, 0.50f, 1.0f};
    bg_color.a *= opacity;
    /* Title bar: slightly lighter than bg */
    Color title_bg = bg_color;
    title_bg.r = fminf(1.0f, title_bg.r + 0.04f);
    title_bg.g = fminf(1.0f, title_bg.g + 0.04f);
    title_bg.b = fminf(1.0f, title_bg.b + 0.04f);
    /* Token colours come from syntax_color() (ui/syntax.c) so the editor, the
     * diff dock and Markdown code blocks all speak one palette. */
    Color clr_linenum = border_color;
    clr_linenum.a = 0.6f;
    Color clr_heading __attribute__((unused)) =
        theme ? theme->ansi[12] : (Color){0.5f, 0.75f, 1.0f, 1.0f};
    Color clr_code_bg = bg_color;
    clr_code_bg.r = fmaxf(0.0f, clr_code_bg.r - 0.04f);
    clr_code_bg.g = fmaxf(0.0f, clr_code_bg.g - 0.04f);
    clr_code_bg.b = fmaxf(0.0f, clr_code_bg.b - 0.04f);
    (void)sel_color;

    /* Left-edge hairline + soft outward shadow that bleeds into the terminal
     * area, giving the viewer a subtle elevated look (mirror of the sidebar's
     * right-edge shadow). Drawn first so the bg fill paints over the inner
     * half of the shadow gradient. */
    Color edge_clr = border_color; edge_clr.a = fmaxf(edge_clr.a, 0.55f);
    renderer_draw_rrect(r,
        x, y, 1.0f, h,
        edge_clr,
        0.0f, 0.0f, 0.0f, 0.0f,
        10.0f * dpi, 0.22f, -5.0f * dpi, 0.0f);

    /* Background */
    renderer_draw_rect(r, x, y, w, h, bg_color);

    f32 title_h = VIEWER_TITLE_H_PT * dpi;
    f32 content_y = y + title_h + 4;
    f32 content_h = h - title_h - 8;
    i32 visible_lines = (i32)(content_h / ch);

    if (fb->view_mode == FVIEW_IMAGE && fb->view_image_rgba &&
        fb->view_image_w > 0 && fb->view_image_h > 0) {
        /* Advance animated GIF playback from elapsed time rather than
         * render count so display refresh throttling does not change
         * animation speed. */
        if (fb->view_image_is_animated && fb->view_image_frame_count > 1 &&
            fb->view_image_frame_delays) {
            f64 now = platform_time_sec();

            /* Total loop duration — one full pass through every frame. */
            f64 loop_total = 0.0;
            for (i32 i = 0; i < fb->view_image_frame_count; i++) {
                i32 ms = fb->view_image_frame_delays[i];
                if (ms <= 0) ms = 100;
                loop_total += (f64)ms / 1000.0;
            }
            if (loop_total < 0.001) loop_total = 0.001;

            /* Elapsed since the current frame_index began playing. If
             * `now` jumps far ahead (sleep/suspend), it is taken modulo
             * loop_total, so the walk never iterates more than one full
             * loop — bounded work regardless of how long the sleep
             * lasted. */
            f64 elapsed = now - fb->view_image_frame_anchor;
            if (elapsed >= loop_total) {
                f64 whole_loops = floor(elapsed / loop_total);
                fb->view_image_frame_anchor += whole_loops * loop_total;
                elapsed -= whole_loops * loop_total;
            }

            /* Walk forward from the current frame, consuming each
             * frame's delay until we find the one that contains `now`.
             * Bounded by frame_count iterations because elapsed is
             * already < loop_total. */
            i32 idx = fb->view_image_frame_index;
            for (i32 guard = 0; guard < fb->view_image_frame_count; guard++) {
                i32 ms = fb->view_image_frame_delays[idx];
                if (ms <= 0) ms = 100;
                f64 dur = (f64)ms / 1000.0;
                if (elapsed < dur) break;
                elapsed -= dur;
                fb->view_image_frame_anchor += dur;
                idx = (idx + 1) % fb->view_image_frame_count;
            }
            if (idx != fb->view_image_frame_index) {
                fb->view_image_frame_index = idx;
                fb->view_image_gen++;
            }

            /* Schedule the main loop's wake-up at the next frame
             * boundary so no cycles are spent between frames. */
            i32 cur_ms = fb->view_image_frame_delays[idx];
            if (cur_ms <= 0) cur_ms = 100;
            f64 cur_dur = (f64)cur_ms / 1000.0;
            fb->view_image_next_frame_at = fb->view_image_frame_anchor + cur_dur;
        } else {
            fb->view_image_next_frame_at = 0.0;
        }

        /* Aspect-fit the decoded image into the content area, then apply
         * the user's zoom + pan. dx/dy is the top-left of the drawn image
         * in viewer coords; the draw is scissor-clipped so a zoomed image
         * doesn't spill over the title bar / panel border. */
        f32 area_x = x + pad;
        f32 area_y = content_y;
        f32 area_w = w - 2 * pad;
        f32 area_h = content_h;
        if (area_w < 1.0f) area_w = 1.0f;
        if (area_h < 1.0f) area_h = 1.0f;

        f32 sx = area_w / (f32)fb->view_image_w;
        f32 sy = area_h / (f32)fb->view_image_h;
        f32 fit = sx < sy ? sx : sy;
        f32 zoom = fb->view_image_zoom > 0.0f ? fb->view_image_zoom : 1.0f;
        f32 dw = (f32)fb->view_image_w * fit * zoom;
        f32 dh = (f32)fb->view_image_h * fit * zoom;

        f32 dx = area_x + (area_w - dw) * 0.5f + fb->view_image_pan_x;
        f32 dy = area_y + (area_h - dh) * 0.5f + fb->view_image_pan_y;

        /* Checkerboard so transparent PNGs don't blend into the panel
         * background and look "lost". 12-pixel cells, alpha 0.4. */
        renderer_flush_rects(r);
        renderer_flush_glyphs(r);
        renderer_push_scissor(r, area_x, area_y, area_w, area_h);
        f32 cell = 12.0f * dpi;
        if (cell < 4.0f) cell = 4.0f;
        /* Dark ink on the dark panel is a drawing nobody can see: a drawn SVG
         * whose ink is mostly dark is backed by a light, opaque checkerboard. */
        const bool light_backing = fb->view_image_svg && fb->view_image_ink_dark;
        Color cb_a = light_backing ? (Color){0.93f, 0.93f, 0.94f, 1.0f} : (Color){0.18f, 0.18f, 0.20f, 0.55f};
        Color cb_b = light_backing ? (Color){0.84f, 0.84f, 0.86f, 1.0f} : (Color){0.10f, 0.10f, 0.12f, 0.55f};
        f32 first_x = dx - fmodf(dx - area_x, cell * 2.0f);
        f32 first_y = dy - fmodf(dy - area_y, cell * 2.0f);
        for (f32 yy = first_y; yy < dy + dh; yy += cell) {
            for (f32 xx = first_x; xx < dx + dw; xx += cell) {
                if (xx + cell <= dx || yy + cell <= dy) continue;
                int gx = (int)floorf((xx - first_x) / cell);
                int gy = (int)floorf((yy - first_y) / cell);
                Color cc = ((gx + gy) & 1) ? cb_a : cb_b;
                f32 ox = xx > dx ? xx : dx;
                f32 oy = yy > dy ? yy : dy;
                f32 ow = (xx + cell) - ox;
                f32 oh = (yy + cell) - oy;
                if (ox + ow > dx + dw) ow = (dx + dw) - ox;
                if (oy + oh > dy + dh) oh = (dy + dh) - oy;
                if (ow > 0 && oh > 0)
                    renderer_draw_rect(r, ox, oy, ow, oh, cc);
            }
        }
        renderer_flush_rects(r);

        /* Cached upload: stable key per file (FB pointer XOR generation),
         * so opening another image bumps the generation and invalidates
         * the previous texture without leaking. For animated GIFs the
         * generation also bumps on every frame change so the cache
         * re-uploads the new frame's pixels. */
        const void *key = (const void *)fb;
        usize frame_stride = (usize)fb->view_image_w * (usize)fb->view_image_h * 4u;
        const u8 *frame_px = fb->view_image_rgba +
                             (usize)fb->view_image_frame_index * frame_stride;
        renderer_draw_image_cached(r, key, fb->view_image_gen,
                                   frame_px,
                                   fb->view_image_w, fb->view_image_h,
                                   floorf(dx), floorf(dy),
                                   floorf(dw), floorf(dh));
        renderer_pop_scissor(r);
    } else if (fb->view_mode == FVIEW_MARKDOWN && fb->md_doc && !fb->md_raw_mode) {
        /* Lazily allocate the click-target buffer — only paid the first
         * time a markdown doc is rendered. Freed in fb_destroy. */
        if (!fb->md_link_rects) {
            fb->md_link_rects = (MdLinkRect *)calloc(FB_MD_LINK_CAP,
                                                    sizeof(MdLinkRect));
        }
        if (!fb->md_task_rects) {
            fb->md_task_rects = (MdTaskRect *)calloc(FB_MD_TASK_CAP,
                                                     sizeof(MdTaskRect));
        }
        if (!fb->md_fold_rects) {
            fb->md_fold_rects = (MdFoldRect *)calloc(FB_MD_FOLD_CAP,
                                                     sizeof(MdFoldRect));
        }
        if (!fb->md_glyph_rects) {
            fb->md_glyph_rects = (MdGlyphRect *)calloc(FB_MD_GLYPH_CAP,
                                                       sizeof(MdGlyphRect));
        }
        if (!fb->md_outline) {
            fb->md_outline = (MdOutlineItem *)calloc(FB_MD_OUTLINE_CAP,
                                                     sizeof(MdOutlineItem));
        }
        fb->md_link_count = 0;
        fb->md_task_count = 0;
        fb->md_fold_count = 0;
        /* NB: md_glyph_count is intentionally NOT reset here. md_render clears
         * it at its top (via glyph_rect_count), and fb_md_draw_selection runs
         * just before md_render reading the previous frame's glyph rects to
         * paint the highlight behind the text — zeroing it here would blank
         * the selection. */
        /* Cache the content origin so the mouse/key handlers can map
         * framebuffer coords into content space without re-deriving the
         * (sidebar vs. tab) viewer layout. */
        fb->md_origin_x = x + pad;
        fb->md_origin_y = content_y;
        fb->md_view_h   = content_h;
        MdRenderCtx mctx = {
            .r        = r,
            .theme    = theme,
            .x        = x + pad,
            .y        = content_y,
            .w        = w - 2 * pad,
            .h        = content_h,
            .cw       = cw,
            .ch       = ch,
            .dpi      = dpi,
            .scroll_y = fb->view_scroll_px,
            .images   = fb->md_images,
            .embeds   = fb->md_embeds,
            .base_dir = fb->md_doc->base_dir,
            .src_base = fb->md_src ? (const u8 *)fb->md_src : (const u8 *)fb->view_content,
            .src_len  = (u32)fb->view_size,
            .link_rects      = fb->md_link_rects,
            .link_rect_cap   = fb->md_link_rects ? FB_MD_LINK_CAP : 0u,
            .link_rect_count = fb->md_link_rects ? &fb->md_link_count : NULL,
            .glyph_rects      = fb->md_glyph_rects,
            .glyph_rect_cap   = fb->md_glyph_rects ? FB_MD_GLYPH_CAP : 0u,
            .glyph_rect_count = fb->md_glyph_rects ? &fb->md_glyph_count : NULL,
            .collect_all      = false,
            .outline          = fb->md_outline,
            .outline_cap      = fb->md_outline ? FB_MD_OUTLINE_CAP : 0u,
            .outline_count    = fb->md_outline ? &fb->md_outline_count : NULL,
            .task_rects       = fb->md_task_rects,
            .task_rect_cap    = fb->md_task_rects ? FB_MD_TASK_CAP : 0u,
            .task_rect_count  = fb->md_task_rects ? &fb->md_task_count : NULL,
            .fold_rects       = fb->md_fold_rects,
            .fold_rect_cap    = fb->md_fold_rects ? FB_MD_FOLD_CAP : 0u,
            .fold_rect_count  = fb->md_fold_rects ? &fb->md_fold_count : NULL,
            /* Pending "#Heading" / "#^block" jump from a wikilink click. The
             * block's document-space y is only known once the renderer has
             * walked to it, so the seek is armed here and applied below. */
            .seek_block       = fb->md_seek_block,
            .seek_block_y     = (fb->md_seek_block != MD_SEEK_NONE)
                                    ? &fb->md_seek_y : NULL,
            /* Chrome inside the document that reacts to the pointer: the
             * fenced blocks' Copy buttons. The rects they publish are handed
             * to ui_hover_hit() below so the pointer gate repaints when the
             * pointer crosses one; a hover drawn without that entry never
             * appears. */
            .hover_x          = hover_x,
            .hover_y          = hover_y,
            .copy_flash_block = fb->md_copy_flash_src,
            .copy_flash       = fb_md_copy_flash(fb, platform_time_sec()),
        };
        /* Clip markdown output to the viewer body. Without this, blocks
         * scrolled past the top render into the terminal/sidebar behind
         * the panel — visible as a faint "watermark" of older content
         * because the rest of the UI didn't paint over those pixels. */
        renderer_flush_rects(r);
        renderer_flush_glyphs(r);
        renderer_push_scissor(r, x, content_y, w, content_h);
        /* Selection highlight FIRST, so it paints behind this frame's text.
         * md_render flushes its own glyph batch before returning, so pushing
         * the highlight after it would draw on top and dim the text. md_render
         * clears the glyph-rect collector only at its top, so the rects from
         * the previous frame are still intact here. Index mapping uses the
         * captured md_glyph_scroll_px (see md_sel_index_at) so endpoints land
         * on the right glyphs even mid-scroll; the visual highlight still
         * lags one frame in framebuffer pixels, which is imperceptible. */
        fb_md_draw_selection(fb, r, theme);
        fb_find_draw_matches(fb, r, theme);   /* find highlights, also behind text */
        renderer_flush_rects(r);
        /* The copy frame: every block is drawn (the scissor above clips the
         * off-screen ones) into a whole-document glyph list sized from the
         * source — every glyph comes from a source byte or from block chrome
         * (bullets, numbers, rules), which the slack covers. */
        MdGlyphRect *copy_all = NULL;
        u32 copy_all_count = 0;
        if (fb->md_copy_pending && !fb->md_glyph_rects) fb->md_copy_pending = false;
        if (fb->md_copy_pending) {
            usize want = (usize)fb->view_size + 16384u;
            if (want > 4u * 1024u * 1024u) want = 4u * 1024u * 1024u;
            copy_all = (MdGlyphRect *)calloc(want, sizeof(MdGlyphRect));
            if (copy_all) {
                mctx.glyph_rects      = copy_all;
                mctx.glyph_rect_cap   = (u32)want;
                mctx.glyph_rect_count = &copy_all_count;
                mctx.collect_all      = true;
            } else {
                fb->md_copy_pending = false;
            }
        }
        if (fb->md_restore_read_scroll) {
            MdRenderCtx measure = mctx;
            measure.measure_only = true;
            measure.measure_exact = true;
            f32 total = md_render(fb->md_doc, &measure);
            fb->view_scroll_px = fmaxf(0.0f, fminf(fb->view_scroll_px, total - content_h));
            mctx.scroll_y = fb->view_scroll_px;
            fb->md_restore_read_scroll = false;
        }
        mctx.table_scroll_x = fb->md_table_scroll_x;
        fb->md_table_overflow = 0;
        mctx.table_overflow = &fb->md_table_overflow;
        fb->view_content_px = md_render(fb->md_doc, &mctx);
        if (copy_all) {
            fb_md_copy_finish(fb, copy_all, copy_all_count, content_y, content_h);
            free(copy_all);
        }
        /* Publish this frame's Copy buttons to the pointer-damage registry.
         * The renderer cannot do it — it is deliberately free of ui/anim — and
         * the rects only exist once it has run, so the registration happens
         * here, from the rects it just recorded. Without it the button's hover
         * state is drawn but never repainted into: the loop stopped producing
         * frames for raw pointer motion. */
        for (u32 li = 0; li < fb->md_link_count; li++) {
            const MdLinkRect *L = &fb->md_link_rects[li];
            if (L->kind == MD_LINKRECT_COPY)
                (void)ui_hover_hit(L->x, L->y, L->w, L->h, hover_x, hover_y);
        }
        /* Apply a pending anchor jump now that the block's y is known, and
         * disarm it — one seek per navigation, not one per frame. */
        if (fb->md_seek_block != MD_SEEK_NONE) {
            f32 target = fb->md_seek_y - 8.0f;
            if (target < 0.0f) target = 0.0f;
            f32 max_y = fb->view_content_px - content_h;
            if (max_y < 0.0f) max_y = 0.0f;
            if (target > max_y) target = max_y;
            fb->view_scroll_px = target;
            fb->md_seek_block  = MD_SEEK_NONE;
        }
        /* Record the scroll the rects were captured at so the next
         * md_sel_index_at call converts them back to doc space using THIS
         * frame's scroll, not whatever it has drifted to by then. */
        fb->md_glyph_scroll_px = fb->view_scroll_px;
        renderer_flush_rects(r);
        renderer_flush_glyphs(r);
        renderer_pop_scissor(r);
    } else if (fb->view_mode == FVIEW_MARKDOWN && fb->md_raw_mode) {
        /* ---- Markdown Live Preview editor ----
         * Each SOURCE line is drawn independently: the line holding the cursor
         * is shown raw (syntax markers visible, dimmed) with the caret, every
         * short inactive lines are styled with markers hidden. Long prose wraps
         * as source; fences stay unwrapped. The buffer keeps source line/col. */
        f32 lx = x + pad;
        f32 maxx = x + w - pad;
        Color fg   = theme ? theme->fg : (Color){0.86f,0.86f,0.90f,1.0f};
        Color dimc = { fg.r*0.5f, fg.g*0.5f, fg.b*0.5f, fg.a };
        Color hcol = { fminf(1.0f,fg.r+0.12f), fminf(1.0f,fg.g+0.12f), fminf(1.0f,fg.b+0.12f), fg.a };
        Color accent = theme ? theme->ansi[12] : (Color){0.45f,0.62f,1.0f,1.0f};

        fb->md_origin_x = lx; fb->md_origin_y = content_y;
        fb->live_cw = cw; fb->live_ch = ch; fb->live_dpi = dpi;
        fb->live_width = maxx - lx;
        fb->code_cw = cw;
        fb->code_visible_cols = (i32)(fb->live_width / cw);
        fb->code_max_vis_cols = 0;
        fb_live_prepare(fb, content_h);
        fb->md_link_count = 0;   /* no clickable read-mode link rects in edit mode */
        fb->md_task_count = 0;

        renderer_flush_rects(r); renderer_flush_glyphs(r);
        renderer_push_scissor(r, x, content_y, w, content_h);
        fb->ed_area_y = content_y;                 /* a drag past these edges scrolls */
        fb->ed_area_h = content_h;
        /* The selection: every line it crosses is drawn as source (what a copy
         * takes), so its columns are exact. */
        bool live_sel = fb->editor_mode && ed_sel_valid(fb);
        i32 sl0 = 0, sc0 = 0, sl1 = 0, sc1 = 0;
        if (live_sel) ed_sel_order(fb, &sl0, &sc0, &sl1, &sc1);
        Color live_sel_col = theme ? theme->selection : (Color){0.20f, 0.30f, 0.50f, 1.0f};
        live_sel_col.a = 0.50f;

        /* Walk to the first visible line, tracking fenced-code state. Lines
         * are found by LENGTH (a NUL is a byte of the note, not its end), and
         * a line's text stops before the CR of a CRLF. What a line IS, and how
         * tall, comes from ui/md_live_lines.h — the click asks the same. */
        char *p = fb->view_content ? fb->view_content : (char*)"";
        char *doc_end = p + (fb->view_content ? fb->view_size : 0);
        i32 li = 0;
        MdLiveFence fence;
        p = (char *)md_live_walk_to(p, (usize)(doc_end - p), fb->view_scroll, &fence, &li);

        i32 rows_fit = 0;   /* whole lines on screen: what the caret follow scrolls against */
        f32 cy = content_y - fb->live_skip_px;
        for (; p <= doc_end && cy < content_y + content_h; li++) {
            char *eol = memchr(p, '\n', (usize)(doc_end - p));
            i32 ll = eol ? (i32)(eol - p) : (i32)(doc_end - p);
            if (eol && ll > 0 && p[ll - 1] == '\r') ll--;      /* the break's, not the line's */
            MdLiveLine cls = md_live_classify(p, ll, &fence);
            i32 ind = cls.indent;
            const char *t = p + ind; i32 tl = ll - ind;        /* trimmed line */
            bool active = (li == fb->cursor_line);
            bool in_sel = live_sel && li >= sl0 && li <= sl1;
            bool raw = active || in_sel;       /* source, not styled text */

            /* classify */
            i32 hlvl = cls.heading;
            bool is_fence = cls.fence;
            bool code_line = cls.code;
            i32 task = -1;          /* 0 unchecked, 1 checked */
            bool bullet = false, ordered = false, quote = false, hr = false;
            i32 marker_len = 0;     /* bytes of the leading block marker (incl trailing space) */
            i32 onum = 0;
            if (!code_line && !is_fence) {
                if (hlvl) marker_len = ind + hlvl + 1;
                if (!hlvl && tl>=2 && t[0]=='>' ) { quote=true; marker_len=ind+1; if(tl>1&&t[1]==' ')marker_len++; }
                if (!hlvl && !quote && tl>=3 && (t[0]=='-'||t[0]=='*'||t[0]=='_')) {
                    bool allsame=true; for(i32 k=0;k<tl;k++){char ch=t[k]; if(ch!=t[0]&&ch!=' '){allsame=false;break;}}
                    if (allsame) hr=true;
                }
                if (!hlvl && !quote && !hr) {
                    if (tl>=5 && (t[0]=='-'||t[0]=='*'||t[0]=='+') && t[1]==' ' && t[2]=='[' &&
                        (t[3]==' '||t[3]=='x'||t[3]=='X') && t[4]==']' ) {
                        task = (t[3]=='x'||t[3]=='X') ? 1 : 0; marker_len = ind+6;
                    } else if (tl>=2 && (t[0]=='-'||t[0]=='*'||t[0]=='+') && t[1]==' ') {
                        bullet=true; marker_len=ind+2;
                    } else { i32 k=0; while(k<tl&&isdigit((unsigned char)t[k]))k++;
                        if (k>0&&k+1<tl&&t[k]=='.'&&t[k+1]==' '){ordered=true;(void)checked_parse_i32_prefix(t,&onum,NULL);marker_len=ind+k+2;} }
                }
            }
            f32 lcw = md_live_cell_w(hlvl, cw), lch = ch * md_live_scale(hlvl);
            f32 lh = md_live_line_h(hlvl, ch, dpi);

            i32 wrap_cols = md_live_wrap_cols(cls, maxx - lx, cw);
            i32 wraps = md_live_rows(p, ll, wrap_cols);
            if (wraps > 1) {
                /* Wrapped lines stay source-visible, so a selection and a click
                 * use the same byte map even as the caret changes lines. */
                i32 at = 0;
                renderer_set_ui_scale(r, lcw, lch);
                do {
                    i32 stop = md_live_wrap_end(p, ll, at, wrap_cols);
                    if (cy + lh > content_y && cy < content_y + content_h) {
                        if (in_sel) {
                            i32 sb = li == sl0 ? sc0 : 0, eb = li == sl1 ? sc1 : ll;
                            sb = sb > at ? sb : at; eb = eb < stop ? eb : stop;
                            i32 vs = editor_col_at(p + at, stop - at, sb - at, 1);
                            i32 ve = editor_col_at(p + at, stop - at, eb - at, 1);
                            if (stop == ll && li < sl1) ve++;
                            if (ve > vs) renderer_draw_rect(r, lx + vs * lcw, cy, (ve - vs) * lcw, lch, live_sel_col);
                        }
                        if (fb->find_active && fb->find_match_count)
                            fb_find_draw_line(fb, r, (usize)(p + at - fb->view_content), stop - at, lx, cy, lcw, lch, theme);
                        fb_live_literal(r, p + at, stop - at, 0, lx, cy, lcw, fg, dimc);
                        if (active && fb->cursor_col >= at && (fb->cursor_col < stop || stop == ll)) {
                            i32 vc = editor_col_at(p + at, stop - at, fb->cursor_col - at, 1);
                            f32 cx = lx + vc * lcw;
                            if (!fb->kbd_away) {
                                renderer_flush_glyphs(r);
                                Color cursor = theme ? theme->cursor : (Color){0.8f, 0.8f, 0.85f, 0.9f};
                                renderer_draw_rect(r, cx, cy, fmaxf(2.0f, 2.0f*dpi), lch, cursor);
                                fb->caret_drawn = true; fb->caret_x = cx; fb->caret_y = cy;
                                fb->caret_cw = lcw; fb->caret_ch = lch;
                            }
                            fb->caret_px_x = cx; fb->caret_px_y = cy; fb->caret_px_h = lch;
                        }
                    }
                    cy += lh; at = stop; rows_fit++;
                } while (at < ll);
                if (!eol) break;
                p = eol + 1;
                continue;
            }
            f32 saved_lx = lx;
            if (code_line || is_fence) {
                i32 columns = editor_cols(p, ll, 1);
                if (columns > fb->code_max_vis_cols) fb->code_max_vis_cols = columns;
                lx -= fb->view_scroll_x * lcw;
            }

            if (cy + lh < content_y) { cy += lh; lx = saved_lx; if (!eol) break; p = eol+1; continue; }  /* above view */
            if (cy + lh <= content_y + content_h + 0.5f) rows_fit++;

            renderer_set_ui_scale(r, lcw, lch);
            f32 indent_px = (f32)(ind) * cw + (quote ? 10.0f*dpi : 0.0f);

            /* Find-match highlight for this raw source line (behind the text).
             * Raw lines render every codepoint at one cell, so byte offsets map
             * straight to columns. */
            if (fb->find_active && fb->find_match_count)
                fb_find_draw_line(fb, r, (usize)(p - fb->view_content), ll,
                                  lx, cy, lcw, lch, theme);

            if (code_line)                                         /* the block's tint, caret line included */
                renderer_draw_rect(r, lx-2.0f*dpi, cy-1.0f*dpi, maxx-lx+4.0f*dpi, lch+2.0f*dpi, (Color){1,1,1,0.05f});

            if (in_sel) {
                i32 vs = 0, ve = 0;
                i32 sb = li == sl0 ? sc0 : 0, eb = li == sl1 ? sc1 : ll;
                for (i32 k = 0; k < ll; ) {
                    if (k < sb) vs++;
                    if (k < eb) ve++;
                    u32 cpq; u32 nbq = utf8_decode((const u8*)p + k, (usize)(ll - k), &cpq);
                    k += nbq ? (i32)nbq : 1;
                }
                if (li < sl1) ve++;                              /* the break is selected */
                f32 sx0 = lx + (f32)vs * lcw, sx1 = lx + (f32)ve * lcw;
                if (sx1 > maxx) sx1 = maxx;
                if (sx1 > sx0) renderer_draw_rect(r, sx0, cy, sx1 - sx0, lch, live_sel_col);
            }

            if (raw) {
                /* Raw source: markers visible but colored (dimmed markers,
                 * tinted text) so the edited line reads like highlighted source
                 * instead of a flat run, + caret. A fence and the code inside
                 * one are not Markdown: their bytes, as they are. */
                if (code_line || is_fence) {
                    i32 marks = 0;
                    if (is_fence) while (marks < tl && t[marks] == t[0]) marks++;
                    fb_live_literal(r, p, ll, is_fence ? ind + marks : 0, lx, cy, lcw, fg, dimc);
                } else {
                    fb_live_inline_raw(r, theme, p, ll, marker_len, lx, cy, lcw, lch, fg);
                }
                if (active) {
                i32 cb = fb->cursor_col; if (cb < 0) cb = 0; if (cb > ll) cb = ll;
                /* cursor_col is a BYTE offset; the caret's visual column is the
                 * count of codepoints before it (a multi-byte char is 1 column). */
                i32 vcol = 0;
                for (i32 k = 0; k < cb; ) {
                    u32 cp2; u32 nb2 = utf8_decode((const u8*)p+k,(usize)(ll-k),&cp2);
                    k += nb2 ? (i32)nb2 : 1; vcol++;
                }
                renderer_flush_glyphs(r);
                Color cur = theme ? theme->cursor : (Color){0.8f,0.8f,0.85f,0.9f}; cur.a = 0.9f;
                /* A caret says "keys land here" — not while they land in the
                 * terminal (FileBrowser.kbd_away). */
                if (!fb->kbd_away) {
                    renderer_draw_rect(r, lx + (f32)vcol * lcw, cy, fmaxf(2.0f,2.0f*dpi), lch, cur);
                    fb->caret_drawn = true;
                    fb->caret_x = lx + (f32)vcol * lcw;
                    fb->caret_y = cy;
                    fb->caret_cw = lcw;
                    fb->caret_ch = lch;
                }
                /* Remember the caret pixel position so the autocomplete popup
                 * can anchor under it next frame. */
                fb->caret_px_x = lx + (f32)vcol * lcw;
                fb->caret_px_y = cy;
                fb->caret_px_h = lch;
                }
            } else if (is_fence || code_line) {
                if (!is_fence)                                     /* code interior (mono, dim) */
                    fb_live_literal(r, p, ll, 0, lx, cy, cw, dimc, dimc);
                /* fence line itself: hidden */
            } else if (hr) {
                renderer_draw_rect(r, lx, cy + lch*0.5f, maxx - lx, fmaxf(1.0f,1.0f*dpi), dimc);
            } else if (hlvl) {
                fb_live_inline(r, theme, t + (marker_len-ind), tl - (marker_len-ind), lx, cy, lcw, lch, hcol);
            } else if (task >= 0) {
                Color box = task ? accent : dimc;
                renderer_push_glyph(r, lx+indent_px, cy, task ? 0x2611 : 0x2610, box);  /* ☑ / ☐ */
                fb_live_inline(r, theme, t+(marker_len-ind), tl-(marker_len-ind),
                               lx+indent_px+lcw*1.5f, cy, cw, ch, fg);
            } else if (bullet) {
                renderer_push_glyph(r, lx+indent_px, cy, 0x2022, accent);              /* • */
                fb_live_inline(r, theme, t+(marker_len-ind), tl-(marker_len-ind),
                               lx+indent_px+lcw*1.5f, cy, cw, ch, fg);
            } else if (ordered) {
                char num[12]; i32 nn = snprintf(num, sizeof num, "%d.", onum);
                for (i32 k=0;k<nn;k++) renderer_push_glyph(r, lx+indent_px+(f32)k*cw, cy, (u32)num[k], accent);
                fb_live_inline(r, theme, t+(marker_len-ind), tl-(marker_len-ind),
                               lx+indent_px+(f32)(nn+1)*cw, cy, cw, ch, fg);
            } else if (quote) {
                renderer_draw_rect(r, lx, cy, 3.0f*dpi, lch, accent);
                fb_live_inline(r, theme, t+(marker_len-ind), tl-(marker_len-ind), lx+indent_px, cy, cw, ch, dimc);
            } else {
                fb_live_inline(r, theme, t, tl, lx+indent_px, cy, cw, ch, fg);
            }

            cy += lh;
            lx = saved_lx;
            if (!eol) break;
            p = eol + 1;
        }
        /* The caret follow (fb_editor_move, typing) scrolls against the rows the
         * last render had room for, so this view records them in
         * code_visible_lines. */
        /* A note shorter than the viewer ends with room under its last line, and that
         * is room for rows: counting only the lines drawn made the caret follow
         * scroll a note that fits whole — Cmd+Down on eight lines hid the first. */
        if (cy < content_y + content_h) {
            f32 body = md_live_line_h(0, ch, dpi);
            if (body > 0.0f) rows_fit += (i32)((content_y + content_h - cy) / body);
        }
        fb->code_visible_lines = rows_fit > 0 ? rows_fit : 1;
        renderer_flush_rects(r); renderer_flush_glyphs(r);
        renderer_pop_scissor(r);
        renderer_reset_ui_scale(r);

    } else {
        /* ---- Code / plain text ----
         *
         * Laid out like an editor pane rather than a text dump: a shaded
         * line-number gutter with its own rule, the caret's line washed
         * across the full width, indent guides down the leading whitespace,
         * and a status bar pinned to the bottom edge.
         *
         * The status bar eats into the drawable height, so `visible_lines`
         * is recomputed here — the shared value above still describes the
         * markdown/raw paths, which have no bar. */
        /* The code grid IS the terminal grid. The viewer's 8x16 UI cell is a
         * chrome scale: the glyph quad is anchored to the atlas aspect, so at
         * 16 pt a JetBrains Mono line box held a 17.4 pt glyph and the lines
         * sat on each other — while vim, one tab over, drew the same file at
         * the font's own cell. Taking the font's cell width AND height here
         * gives the editor the exact line pitch and advance the terminal has,
         * so the two read as one surface; the chrome (title, status, scroll
         * bar) keeps the UI scale, restored below. */
        const f32 ui_cw = cw, ui_ch = ch;
        if (r->font.cell_width > 1.0f && r->font.cell_height > 1.0f) {
            cw = r->font.cell_width;
            ch = r->font.cell_height;
            renderer_set_ui_scale(r, cw, ch);
        }
        f32 status_h = VIEWER_STATUS_H_PT * dpi;
        f32 status_y = y + h - status_h;          /* flush with the bottom edge */
        f32 code_h   = status_y - content_y;
        if (code_h < ch) code_h = ch;
        visible_lines = (i32)(code_h / ch);
        if (visible_lines < 1) visible_lines = 1;
        /* What the caret follow scrolls against: the rows this render has
         * room for. */
        fb->code_visible_lines = visible_lines;

        u32 syn_st = 0;
        i32 line_num = 0;
        char *line = fb_code_view_start(fb, fb->view_scroll, &syn_st, &line_num);

        /* The gutter widens with the file: a 5-cell column truncated line
         * numbers the moment a file passed 9999 lines (stb_image.h is ~7.9k
         * and its neighbours are longer still). Size it to the real digit
         * count so the numbers stay right-aligned against the rule. */
        i32 total_lines = fb->view_line_count > 0 ? fb->view_line_count : 1;
        i32 digits = 1;
        for (i32 n = total_lines; n >= 10; n /= 10) digits++;
        if (digits < CODE_GUTTER_MIN_DIGITS) digits = CODE_GUTTER_MIN_DIGITS;
        f32 gutter = ((f32)digits + CODE_GUTTER_PAD_COLS) * cw;
        f32 gutter_rule_x = x + gutter;
        f32 text_x = gutter_rule_x + 8.0f * dpi;  /* x of visual column view_scroll_x */
        f32 hshift = (f32)fb->view_scroll_x * cw;
        i32 max_vis_len = 0;                  /* longest visible line (h-scroll clamp) */

        /* Cache the content geometry for click→caret mapping and the
         * horizontal caret follow (see fb_editor_click_caret). */
        fb->code_text_x = text_x;
        fb->code_text_y = content_y;
        fb->code_cw = cw;
        fb->code_ch = ch;
        fb->ed_area_y = content_y;                 /* a drag past these edges scrolls */
        fb->ed_area_h = (f32)visible_lines * ch;
        fb->code_visible_cols = (i32)((x + w - pad - text_x) / cw);
        if (fb->code_visible_cols < 1) fb->code_visible_cols = 1;

        /* Gutter slab + its rule, drawn once outside the per-line loop.
         * Both are flat rects and go down before any glyph so the numbers
         * sit on top of the slab. */
        ChromePalette code_cp = chrome_palette_for(theme);
        Color gutter_bg = code_cp.is_light
            ? (Color){ bg_color.r - 0.030f, bg_color.g - 0.030f, bg_color.b - 0.030f, bg_color.a }
            : (Color){ bg_color.r + 0.018f, bg_color.g + 0.018f, bg_color.b + 0.022f, bg_color.a };
        renderer_draw_rect(r, x, content_y, gutter, code_h, gutter_bg);
        Color gutter_rule = border_color;
        gutter_rule.a = fminf(0.55f, gutter_rule.a) * opacity;
        renderer_draw_rect(r, gutter_rule_x, content_y, 1.0f, code_h, gutter_rule);

        /* Caret line wash — the single strongest "this is an editor" cue.
         * Full-bleed across gutter and text so the row reads as one band,
         * and only in edit mode: a read-only viewer has no caret to point at.
         * Derived from fg (not a literal) so it stays visible on light
         * themes, where a lightening wash would vanish. */
        i32 caret_row = fb->cursor_line - fb->view_scroll;
        if (fb->editor_mode && caret_row >= 0 && caret_row < visible_lines) {
            Color wash = { fg_color.r, fg_color.g, fg_color.b,
                           (code_cp.is_light ? 0.055f : 0.045f) * opacity };
            renderer_draw_rect(r, x, content_y + caret_row * ch, w, ch, wash);
        }

        /* Invalid UTF-8 and NUL bytes render as U+FFFD and U+2400, so warm those
         * glyphs separately from the source text. */
        if (fb_view_encoding(fb) != EDITOR_ENC_UTF8)
            font_warm_text_glyphs(&r->font, "\xEF\xBF\xBD\xE2\x90\x80");

        /* The lines are found by LENGTH: a NUL is a byte of the file, not its
         * end, and the lines after it are drawn too. */
        const char *doc_end = fb->view_content + fb->view_size;
        /* The selection, behind the text of every line it crosses. */
        bool sel_draw = fb->editor_mode && ed_sel_valid(fb);
        i32 sl0 = 0, sc0 = 0, sl1 = 0, sc1 = 0;
        if (sel_draw) ed_sel_order(fb, &sl0, &sc0, &sl1, &sc1);
        Color sel_col = theme ? theme->selection : (Color){0.20f, 0.30f, 0.50f, 1.0f};
        sel_col.a = 0.50f * opacity;
        for (i32 vi = 0; vi < visible_lines && line && line < doc_end; vi++) {
            usize line_off = (usize)(line - fb->view_content);
            usize brk = editor_line_break(fb->view_content, fb->view_size, line_off);
            /* The line's TEXT: up to its break, and not the CR of a CRLF,
             * which is half of the break and never a cell. */
            i32 len = (i32)(editor_line_text_end(fb->view_content, fb->view_size,
                                                 line_off, brk) - line_off);
            f32 ly = content_y + vi * ch;
            line_num++;
            i32 line_cols = editor_cols(line, len, FB_TAB_COLS);
            if (line_cols > max_vis_len) max_vis_len = line_cols;

            /* Warm only the visible UTF-8 slice (plus the partial edge cell)
             * before emitting glyphs. A long off-screen suffix needs lexing,
             * but must not rasterize thousands of invisible characters. */
            FbCodeSlice slice = fb_code_visible_slice(line, len, fb->view_scroll_x,
                                                      fb->code_visible_cols + 1);
            font_warm_text_glyphs_n(&r->font, line + slice.begin, (usize)(slice.end - slice.begin));

            /* Line number, right-aligned against the rule. The caret's own
             * number comes up to full strength while the rest stay quiet —
             * the reader can find their position without counting rows. */
            bool is_caret_line = fb->editor_mode && (vi == caret_row);
            char lnbuf[16];
            snprintf(lnbuf, sizeof(lnbuf), "%*d", digits, line_num);
            Color ln_clr = is_caret_line
                ? (Color){ fg_color.r, fg_color.g, fg_color.b, 0.88f * opacity }
                : clr_linenum;
            draw_text_clipped(r, lnbuf,
                              gutter_rule_x - CODE_GUTTER_PAD_COLS * 0.5f * cw
                                            - (f32)digits * cw,
                              ly, ln_clr, (f32)digits * cw + cw);

            /* Indent guides. Drawn only across the leading whitespace, so a
             * guide never cuts through a glyph, and skipped on blank lines
             * (a run of guides down an empty line is visual noise, not
             * structure). A tab runs to the next stop — the same cell walk
             * the text below is drawn with, so the guides end where the
             * first glyph begins. */
            {
                i32 indent_cols = 0;
                i32 wi = 0;
                while (wi < len && (line[wi] == ' ' || line[wi] == '\t')) {
                    indent_cols += editor_cell_at(line, len, wi, indent_cols, FB_TAB_COLS).cols;
                    wi++;
                }
                if (wi < len) {   /* not a blank/whitespace-only line */
                    Color guide = { fg_color.r, fg_color.g, fg_color.b, 0.10f * opacity };
                    for (i32 gc = CODE_INDENT_GUIDE_COLS; gc < indent_cols;
                         gc += CODE_INDENT_GUIDE_COLS) {
                        f32 gx = text_x + (f32)(gc - fb->view_scroll_x) * cw;
                        if (gx < text_x - 0.5f) continue;       /* scrolled into the gutter */
                        if (gx > x + w - pad) break;
                        renderer_draw_rect(r, gx, ly, 1.0f, ch, guide);
                    }
                }
            }

            /* The selection on this line: from its start column (or the line's
             * own) to its end (or one cell past the text when the break is
             * selected too), in the cells the text is drawn in. */
            if (sel_draw) {
                i32 abs_line = fb->view_scroll + vi;
                if (abs_line >= sl0 && abs_line <= sl1) {
                    i32 vs = editor_col_at(line, len, abs_line == sl0 ? sc0 : 0, FB_TAB_COLS);
                    i32 ve = editor_col_at(line, len, abs_line == sl1 ? sc1 : len, FB_TAB_COLS);
                    if (abs_line < sl1) ve += 1;
                    f32 sx0 = text_x + (f32)(vs - fb->view_scroll_x) * cw;
                    f32 sx1 = text_x + (f32)(ve - fb->view_scroll_x) * cw;
                    if (sx0 < text_x) sx0 = text_x;
                    if (sx1 > x + w - pad) sx1 = x + w - pad;
                    if (sx1 > sx0) renderer_draw_rect(r, sx0, ly, sx1 - sx0, ch, sel_col);
                }
            }

            /* Code content, coloured from the shared scanner. The WHOLE line
             * is lexed even when only part of it is on screen: the state word
             * it hands to the next line has to be complete, or an off-screen
             * quote would leave every line below it painted as a string.
             *
             * The visible slice uses codepoint columns; token spans retain
             * their full-line byte offsets so horizontal scrolling preserves
             * both colours and the next line's continuation state. */
            SynSpan spans[SYN_MAX_SPANS];
            i32 nspans = 0;
            if (fb->code_lang != SYN_LANG_NONE)
                nspans = syntax_scan(fb->code_lang, line, (u32)len, &syn_st,
                                     spans, SYN_MAX_SPANS);

            f32 cx = text_x - hshift + (f32)slice.column * cw;
            i32 ci = slice.begin;
            i32 col = slice.column;
            i32 si = 0;

            while (ci < len && cx < x + w - pad) {
                /* Spans arrive in order and never overlap, so one forward
                 * cursor over them is enough for the whole line. */
                while (si < nspans &&
                       (i32)(spans[si].off + spans[si].len) <= ci) si++;
                Color cc = fg_color;
                if (si < nspans && ci >= (i32)spans[si].off) {
                    cc = syntax_color(theme, (SynKind)spans[si].kind);
                    cc.a *= opacity;
                }

                /* One cell at a time, through the walk the caret and the
                 * click use (ui/editor_text.h): a multi-byte character is one
                 * glyph, a tab runs to its stop, and a byte that is not UTF-8
                 * is drawn (U+FFFD) in a column of its own — the same column
                 * the caret counts, so a Latin-1 file keeps its accented
                 * letters and the caret stays on them. */
                EditorCell cell = editor_cell_at(line, len, ci, col, FB_TAB_COLS);
                if (cell.cp && cx >= text_x - 0.5f)
                    renderer_push_glyph(r, cx, ly, cell.cp, cc);
                cx += (f32)cell.cols * cw;
                col += cell.cols;
                ci += cell.bytes;
            }

            line = fb->view_content + (brk < fb->view_size ? brk + 1 : brk);
        }
        fb->code_max_vis_cols = max_vis_len;

        /* Editor cursor */
        if (fb->editor_mode) {
            i32 cline = fb->cursor_line - fb->view_scroll;
            if (cline >= 0 && cline < visible_lines) {
                i32 line_len = editor_line_len(fb, fb->cursor_line);
                i32 cur_col = fb->cursor_col;
                if (cur_col < 0) cur_col = 0;
                if (cur_col > line_len) cur_col = line_len;
                /* cursor_col is a byte offset — the visual column is the sum
                 * of the cells before it, from the painter's own walk. */
                const char *cls = editor_line_start(fb, fb->cursor_line);
                i32 vcol = editor_col_at(cls, line_len, cur_col, FB_TAB_COLS);
                f32 cur_x = text_x + (f32)(vcol - fb->view_scroll_x) * cw;
                f32 cur_y = content_y + cline * ch;
                Color cur_clr = theme ? theme->cursor : (Color){0.8f, 0.8f, 0.85f, 0.9f};
                cur_clr.a = 0.9f;
                /* Off-screen left: don't draw in the gutter. And no caret while
                 * the keys land in the terminal (FileBrowser.kbd_away). */
                if (vcol >= fb->view_scroll_x && !fb->kbd_away) {
                    renderer_draw_rect(r, cur_x, cur_y, 2.0f * dpi, ch, cur_clr);
                    fb->caret_drawn = true;
                    fb->caret_x = cur_x;
                    fb->caret_y = cur_y;
                    fb->caret_cw = cw;
                    fb->caret_ch = ch;
                }
            }
        }

        /* ---- Status bar ----
         *
         * Bottom rail carrying what an editor is expected to answer without
         * being asked: where the caret is, how big the file is, what it is
         * being read as. Read-only mode has no caret, so it reports the line
         * count instead of Ln/Col rather than showing a stale position.
         *
         * Flat rects flush UNDER rrects and glyphs (see the title bar's note
         * on layering), so the fill and its rule go down before the labels. */
        {
            Color status_bg = code_cp.is_light
                ? (Color){ bg_color.r - 0.060f, bg_color.g - 0.060f, bg_color.b - 0.058f, bg_color.a }
                : (Color){ bg_color.r + 0.040f, bg_color.g + 0.040f, bg_color.b + 0.048f, bg_color.a };
            renderer_draw_rect(r, x, status_y, w, status_h, status_bg);
            Color status_rule = border_color;
            status_rule.a = fminf(0.80f, fmaxf(0.45f, status_rule.a)) * opacity;
            renderer_draw_rect(r, x, status_y, w, 1.0f, status_rule);

            /* Status labels are chrome: back to the viewer's UI cell. */
            cw = ui_cw; ch = ui_ch;
            renderer_flush_glyphs(r);
            renderer_set_ui_scale(r, cw, ch);
            f32 slabel_y = renderer_ui_cap_center_y(r, status_y, status_h);
            Color s_fg  = { fg_color.r, fg_color.g, fg_color.b, 0.70f * opacity };
            Color s_dim = { fg_color.r, fg_color.g, fg_color.b, 0.46f * opacity };

            /* Left: caret position (edit) or size of the document (read). */
            char left_buf[64];
            if (fb->editor_mode) {
                /* Columns are 1-based and are the caret's own (the cell walk
                 * above) — a byte offset would read as column 4 in the middle
                 * of a Turkish "ş", and a count of characters as column 2
                 * after a tab the caret stands four cells past. */
                const char *cls = editor_line_start(fb, fb->cursor_line);
                i32 line_len = editor_line_len(fb, fb->cursor_line);
                i32 cur_col = fb->cursor_col;
                if (cur_col < 0) cur_col = 0;
                if (cur_col > line_len) cur_col = line_len;
                i32 vcol = editor_col_at(cls, line_len, cur_col, FB_TAB_COLS);
                snprintf(left_buf, sizeof left_buf, "Ln %d, Col %d",
                         fb->cursor_line + 1, vcol + 1);
            } else if (fb->view_mode == FVIEW_MARKDOWN && !fb->md_doc) {
                snprintf(left_buf, sizeof left_buf, "Source \xc2\xb7 %s",
                         fb->md_parse_job ? "loading preview" : "preview unavailable");
            } else {
                snprintf(left_buf, sizeof left_buf, "%d line%s",
                         total_lines, total_lines == 1 ? "" : "s");
            }
            f32 status_pad = VIEWER_TITLE_PAD_PT * dpi;
            draw_text_clipped(r, left_buf, x + status_pad, slabel_y, s_fg,
                              w - 2.0f * status_pad);

            /* Right: language · line ending · encoding, right-aligned. The
             * separators are dimmer than the fields so the group reads as
             * three items rather than one long string. */
            const char *lang = fb_language_label(fb->view_path);
            /* The ending is the one the file was read with — what Enter
             * writes — and the encoding is what the bytes ARE
             * (fb_view_encoding), so a Windows-1254 file or one full of
             * NULs is not labelled UTF-8. */
            const char *eol_lbl = fb->view_crlf ? "CRLF" : "LF";
            char right_buf[80];
            snprintf(right_buf, sizeof right_buf, "%s  \xc2\xb7  %s  \xc2\xb7  %s",
                     lang, eol_lbl, editor_encoding_label(fb_view_encoding(fb)));
            u32 right_cps = 0;
            for (const u8 *q = (const u8 *)right_buf; *q; q++)
                if ((*q & 0xC0) != 0x80) right_cps++;
            f32 right_w = (f32)right_cps * cw;
            f32 right_x = x + w - status_pad - right_w;
            /* Only when it clears the left label — a narrow viewer drops the
             * right group rather than letting the two collide. */
            f32 left_w = (f32)strlen(left_buf) * cw;
            if (right_x > x + status_pad + left_w + 2.0f * cw) {
                draw_text_clipped(r, right_buf, right_x, slabel_y, s_dim, right_w + cw);
            }
        }
    }

    /* Sticky title bar — drawn AFTER markdown / code content so a partial
     * top-line bleed during scroll gets overdrawn rather than peeking
     * through (the renderer has no GPU scissor for the viewport). The bar
     * is fixed at y=top of the viewer regardless of scroll position. */
    {
        const char *fname = strrchr(fb->view_path, '/');
        fname = fname ? fname + 1 : fb->view_path;

        renderer_flush_rects(r);
        renderer_flush_glyphs(r);
        /* Pin the chrome glyph scale to the viewer's 8x16 explicitly. The
         * markdown-edit branch ends with renderer_reset_ui_scale (→ the atlas's
         * natural ~25x49 metrics), so without this the title text (filename,
         * [e] badge, Read/Edit toggle) rendered oversized in edit mode while
         * read mode — which leaves the scale at 8x16 — looked correct. */
        renderer_set_ui_scale(r, cw, ch);
        f32 title_pad = VIEWER_TITLE_PAD_PT * dpi;
        f32 close_w = VIEWER_CLOSE_W_PT * dpi;
        f32 close_x = x + w - close_w;
        const FbViewerToggle toggle_kind = fb_viewer_toggle_kind(fb);
        f32 toggle_w = toggle_kind != FB_TOGGLE_NONE ? VIEWER_TOGGLE_W_PT * dpi : 0.0f;
        memset(fb->viewer_toggle, 0, sizeof fb->viewer_toggle);
        f32 toggle_gap = 6.0f * dpi;
        f32 toggle_x = close_x - toggle_gap - toggle_w;
        f32 actions_x = toggle_w > 0.0f ? toggle_x : close_x;

        renderer_draw_rect(r, x, y, w, title_h, title_bg);
        Color title_line = border_color;
        title_line.a = fminf(0.75f, fmaxf(0.32f, title_line.a * opacity));
        renderer_draw_rect(r, x, y + title_h - 1.0f, w, 1.0f, title_line);

        /* Live word/read-time readout (markdown edit mode only). Right-align it
         * just left of the Read/Edit toggle and dim it so it sits quietly; the
         * filename + unsaved-pip region (info_right) shrinks to make room. */
        f32 info_right = actions_x;
        char  stats_buf[48]; f32 stats_w = 0.0f, stats_x = 0.0f;
        bool  show_stats = false;
        if (fb->editor_mode && fb->view_mode == FVIEW_MARKDOWN) {
            u32 words = 0, chars_ct = 0, rmin = 0;
            fb_editor_doc_stats(fb, &words, &chars_ct, &rmin);
            (void)chars_ct;
            snprintf(stats_buf, sizeof stats_buf, "%u w \xc2\xb7 %u min", words, rmin);
            u32 cps = 0;                       /* glyph count == codepoints, not bytes */
            for (const u8 *q = (const u8 *)stats_buf; *q; q++)
                if ((*q & 0xC0) != 0x80) cps++;
            stats_w = (f32)cps * cw;
            stats_x = actions_x - 8.0f * dpi - stats_w;
            if (stats_x > x + title_pad + 6.0f * cw) {   /* only if it doesn't crowd the name */
                info_right = stats_x - 8.0f * dpi;
                show_stats = true;
            }
        }

        f32 title_text_x = x + title_pad;
        f32 title_max_w = info_right - title_text_x - 8.0f * dpi;
        if (title_max_w < 0.0f) title_max_w = 0.0f;
        /* One baseline for the bar's text — the name, the counts and the
         * switch's labels — with the capitals centred in the bar. */
        f32 text_y = renderer_ui_cap_center_y(r, y, title_h);
        if (show_stats) {
            Color stats_fg = { fg_color.r, fg_color.g, fg_color.b, 0.42f * opacity };
            draw_text_clipped(r, stats_buf, stats_x, text_y, stats_fg, stats_w + cw);
        }
        /* Filename with a dimmed extension: the stem reads at full strength
         * and the ".md" tail recedes, giving the title a touch of typographic
         * hierarchy instead of one flat run. A leading-dot dotfile (".bashrc")
         * has no stem, so draw it whole. */
        {
            f32 name_y = text_y;
            const char *ext = strrchr(fname, '.');
            if (ext && ext != fname) {
                char stem[256];
                usize sl = (usize)(ext - fname);
                if (sl >= sizeof(stem)) sl = sizeof(stem) - 1;
                memcpy(stem, fname, sl);
                stem[sl] = '\0';
                draw_text_clipped(r, stem, title_text_x, name_y, fg_color, title_max_w);
                /* Advance by codepoints, not bytes — draw_text_clipped steps one
                 * cell per codepoint, so a multibyte stem (e.g. Turkish ç/ş/ğ)
                 * would otherwise push the dimmed extension too far right. */
                u32 stem_cps = 0;
                for (const u8 *q = (const u8 *)stem; *q; q++)
                    if ((*q & 0xC0) != 0x80) stem_cps++;
                f32 ext_x   = title_text_x + (f32)stem_cps * cw;
                f32 ext_max = title_max_w - (f32)stem_cps * cw;
                if (ext_max > 0.0f) {
                    Color ext_fg = fg_color;
                    ext_fg.a *= 0.5f;
                    draw_text_clipped(r, ext, ext_x, name_y, ext_fg, ext_max);
                }
            } else {
                draw_text_clipped(r, fname, title_text_x, name_y, fg_color, title_max_w);
            }
        }

        if (toggle_kind != FB_TOGGLE_NONE) {
            /* Segmented control (Read | Edit for a note, Image | Code for an
             * SVG): a sunken pill track with a raised,
             * softly-shadowed thumb behind the active label. The thumb takes a
             * faint accent tint so the active mode reads at a glance and the
             * control feels elevated/native — without the full-saturation
             * accent fill the very first version used, which read as harsh.
             * Segments are content-sized with balanced padding so both labels
             * sit centered. */
            ChromePalette cp = chrome_palette_for(theme);
            Color accent = theme_ui_accent(theme);
            f32 seg_h = title_h - 8.0f * dpi;
            f32 seg_y = y + 4.0f * dpi;
            fb->viewer_toggle[0] = toggle_x; fb->viewer_toggle[1] = seg_y;
            fb->viewer_toggle[2] = toggle_w; fb->viewer_toggle[3] = seg_h;
            const bool svg_toggle  = toggle_kind == FB_TOGGLE_SVG;
            const char *render_lbl = svg_toggle ? "Image" : "Read";
            const char *raw_lbl    = svg_toggle ? "Code"  : "Edit";
            f32 render_w = (f32)strlen(render_lbl) * cw;
            f32 raw_w    = (f32)strlen(raw_lbl) * cw;
            f32 slack = toggle_w - render_w - raw_w;
            if (slack < 8.0f * dpi) slack = 8.0f * dpi;
            f32 render_seg = render_w + slack * 0.5f;
            f32 raw_seg    = toggle_w - render_seg;

            /* Sunken track */
            Color track = cp.surface_sunken;
            track.a = opacity;
            renderer_draw_rrect_simple(r, toggle_x, seg_y, toggle_w, seg_h,
                                       track, seg_h * 0.5f);

            /* Raised, accent-tinted thumb over the active half */
            bool raw_on = svg_toggle ? fb->view_mode == FVIEW_CODE : fb->md_raw_mode;
            f32 thumb_x = raw_on ? toggle_x + render_seg : toggle_x;
            f32 thumb_w = raw_on ? raw_seg : render_seg;
            f32 inset   = 2.0f * dpi;
            f32 thumb_h = seg_h - 2.0f * inset;
            Color thumb = cp.surface_raised;
            f32 tint = 0.16f;
            thumb.r = thumb.r * (1.0f - tint) + accent.r * tint;
            thumb.g = thumb.g * (1.0f - tint) + accent.g * tint;
            thumb.b = thumb.b * (1.0f - tint) + accent.b * tint;
            thumb.a = opacity;
            renderer_draw_rrect(r, thumb_x + inset, seg_y + inset,
                                thumb_w - 2.0f * inset, thumb_h, thumb,
                                thumb_h * 0.5f, thumb_h * 0.5f,
                                thumb_h * 0.5f, thumb_h * 0.5f,
                                5.0f * dpi, 0.30f * opacity, 0.0f, 1.0f * dpi);

            Color active_fg = chrome_legible_on(thumb);
            active_fg.a = 0.96f * opacity;
            Color inactive_fg = { fg_color.r, fg_color.g, fg_color.b, 0.46f * opacity };
            f32 label_y = text_y;
            draw_text_clipped(r, render_lbl,
                              toggle_x + (render_seg - render_w) * 0.5f, label_y,
                              raw_on ? inactive_fg : active_fg, render_w);
            draw_text_clipped(r, raw_lbl,
                              toggle_x + render_seg + (raw_seg - raw_w) * 0.5f, label_y,
                              raw_on ? active_fg : inactive_fg, raw_w);
        }

        /* Close.
         *
         * Same vocabulary as the sidebar header's close (fb_render_sidebar):
         * nothing at rest but a muted glyph — a routine close should not carry
         * a permanent red box — and on approach a soft red rounded chip with
         * the glyph warming into it.
         *
         * The fade runs through ui_hover_amount(), the shared hover engine in
         * ui/anim.h, rather than a binary hot/not test. That is what makes this
         * button feel like the rest of Liu: the pool eases in AND out, keeps
         * the frame loop awake for the duration of the transition, and lets
         * several controls animate independently.
         *
         * Layering: the title bar's background and hairline are flat rects, and
         * flat rects flush UNDER rrects. Commit them before the chip goes down,
         * then flush the chip before the glyph — otherwise the fill swallows
         * the icon it is supposed to sit behind. */
        f32 close_icon = 11.0f * dpi;
        f32 chip_sz = fminf(close_w - 6.0f * dpi, title_h - 9.0f * dpi);
        if (chip_sz < close_icon + 4.0f * dpi) chip_sz = close_icon + 4.0f * dpi;
        f32 chip_x = close_x + (close_w - chip_sz) * 0.5f;
        f32 chip_y = y + (title_h - chip_sz) * 0.5f;

        /* Hover is sampled over the FULL click zone the hit-tester uses
         * (HIT_VIEWER_CLOSE / HIT_FBTAB_VIEWER_CLOSE claim the whole
         * VIEWER_CLOSE_W_PT column across the title bar), not over the inset
         * chip. The chip is only the visual; feedback that lit a smaller area
         * than the button actually accepts would read as a dead edge. */
        f32 close_hov = ui_hover_amount(close_x, y, close_w, title_h,
                                        hover_x, hover_y);

        Color close_fg = { fg_color.r, fg_color.g, fg_color.b, 0.60f * opacity };
        if (close_hov > 0.001f) {
            renderer_flush_rects(r);
            Color chip = { 0.86f, 0.42f, 0.42f, 0.18f * close_hov * opacity };
            renderer_draw_rrect_simple(r, chip_x, chip_y, chip_sz, chip_sz,
                                       chip, 7.0f * dpi);
            renderer_flush_rrects(r);
            /* Warm the glyph toward the sidebar's hovered-close red and bring
             * it fully up, so the feedback still reads on themes where the
             * chip barely registers. */
            Color hot = { 0.98f, 0.52f, 0.52f, 0.95f * opacity };
            close_fg = color_lerp(close_fg, hot, close_hov);
        }
        icon_draw(r, ICON_CLOSE,
                  close_x + (close_w - close_icon) * 0.5f,
                  y + (title_h - close_icon) * 0.5f,
                  close_icon, close_fg);

        /* Unsaved-changes pip — an amber dot after the filename, shown only
         * when the buffer has edits. Edit mode itself is already shown by the
         * toggle. */
        if (fb->editor_mode && fb->modified) {
            f32 name_w = fminf((f32)strlen(fname) * cw, title_max_w);
            f32 dot_d  = 5.0f * dpi;
            f32 dot_x  = title_text_x + name_w + 7.0f * dpi;
            f32 dot_y  = y + (title_h - dot_d) * 0.5f;
            Color pip = theme ? theme->ansi[3] : (Color){ 0.92f, 0.72f, 0.33f, 1.0f };
            pip.a = 0.95f * opacity;
            if (dot_x + dot_d < info_right)
                renderer_draw_rrect_simple(r, dot_x, dot_y, dot_d, dot_d,
                                           pip, dot_d * 0.5f);
        }
    }

    renderer_flush_rects(r);
    renderer_flush_glyphs(r);
    g_ui_step = 0;
    renderer_reset_ui_scale(r);

    /* Find / Replace bar overlays everything at the top of the viewer. */
    if (fb->find_active)
        fb_find_render(fb, r, x, y + (VIEWER_TITLE_H_PT * dpi), w, dpi, theme, opacity);
    /* Autocomplete popup anchored under the caret. */
    if (fb->ac_active)
        fb_ac_render(fb, r, dpi, theme, opacity);
}

/* =========================================================================
 * Editor operations
 * ========================================================================= */

/* Get pointer to start of line N in view_content.
 *
 * Lines are found by LENGTH (editor_line_break), never as C strings: strchr
 * and `while (*p)` stop at the first NUL, and a file can contain one. */
static char *editor_line_start(FileBrowser *fb, i32 line) {
    if (!fb || !fb->view_content) return NULL;
#ifdef FB_LINE_CACHE_TEST
    if (fb->editor_line_cache_reference) {
        usize at = 0;
        for (i32 i = 0; i < line && at < fb->view_size; i++) {
            usize brk = editor_line_break(fb->view_content, fb->view_size, at);
            at = brk < fb->view_size ? brk + 1 : brk;
        }
        return fb->view_content + at;
    }
#endif
    if (line < 0) line = 0;
    if (!fb->editor_line_cache_valid || fb->editor_line_cache_buffer != fb->view_content ||
        fb->editor_line_cache_offset > fb->view_size ||
        (fb->view_line_count > 0 && fb->editor_line_cache_line >= fb->view_line_count)) {
        fb->editor_line_cache_buffer = fb->view_content;
        fb->editor_line_cache_offset = 0;
        fb->editor_line_cache_line = 0;
        fb->editor_line_cache_valid = true;
    }
    usize offset = fb->editor_line_cache_offset;
    i32 current = fb->editor_line_cache_line;
    while (current > line) {
        if (offset) offset--; /* skip the newline preceding this line */
        while (offset && fb->view_content[offset - 1] != '\n') offset--;
        current--;
    }
    while (current < line && offset < fb->view_size) {
        usize brk = editor_line_break(fb->view_content, fb->view_size, offset);
        offset = brk < fb->view_size ? brk + 1 : brk;
        current++;
    }
    fb->editor_line_cache_line = current;
    fb->editor_line_cache_offset = offset;
    return fb->view_content + offset;
}

/* Bytes of the line's TEXT — what the caret can move over. The CR of a CRLF
 * is not among them: it is half of the line break, so End stops before it
 * and no position exists between it and the LF. */
static i32 editor_line_len(FileBrowser *fb, i32 line) {
    char *start = editor_line_start(fb, line);
    if (!start) return 0;
    usize from = (usize)(start - fb->view_content);
    usize brk = editor_line_break(fb->view_content, fb->view_size, from);
    return (i32)(editor_line_text_end(fb->view_content, fb->view_size, from, brk) - from);
}

/* Bytes of the break that ends the line: 2 for CRLF, 1 for LF, 0 for the last
 * line of the text. Joining two lines removes all of them, so Backspace at
 * column 0 or Delete at the end of a CRLF line leaves no CR in the middle of
 * the joined line. */
static i32 editor_line_eol(FileBrowser *fb, i32 line) {
    char *start = editor_line_start(fb, line);
    if (!start) return 0;
    usize from = (usize)(start - fb->view_content);
    usize brk = editor_line_break(fb->view_content, fb->view_size, from);
    return (i32)editor_line_break_len(fb->view_content, fb->view_size, from, brk);
}

static i32 editor_cp_len_at(const char *line, i32 col, i32 line_len);   /* below */

/* `col` itself when it is a character boundary of `line`, else the start of the
 * character it falls inside. cursor_col is a BYTE offset, and a position
 * between the two bytes of ş is a place an edit must never start from.
 *
 * "Boundary" is the one editor_cp_len_at() steps by, so a stray continuation
 * byte or a sequence cut short is a boundary of its own — otherwise the cursor
 * could not step over one and would be snapped back forever. Local: a byte that
 * is not a continuation byte is never inside a sequence, and one that is has
 * its lead within three bytes. */
static i32 editor_snap_boundary(const char *line, i32 col, i32 line_len) {
    if (col <= 0) return 0;
    if (col >= line_len) return line_len;
    if (((unsigned char)line[col] & 0xC0) != 0x80) return col;
    for (i32 back = 1; back <= 3 && back <= col; back++) {
        i32 lead = col - back;
        if (((unsigned char)line[lead] & 0xC0) == 0x80) continue;
        /* The first byte that is not a continuation owns `col` only when its
         * whole, valid sequence reaches it. */
        return editor_cp_len_at(line, lead, line_len) > back ? lead : col;
    }
    return col;
}

static void editor_clamp_cursor(FileBrowser *fb) {
    if (!fb || !fb->view_content) return;
    if (fb->view_line_count < 1) fb->view_line_count = 1;
    if (fb->cursor_line < 0) fb->cursor_line = 0;
    if (fb->cursor_line >= fb->view_line_count) fb->cursor_line = fb->view_line_count - 1;

    i32 line_len = editor_line_len(fb, fb->cursor_line);
    if (fb->cursor_col < 0) fb->cursor_col = 0;
    if (fb->cursor_col > line_len) fb->cursor_col = line_len;
    /* Every edit and every motion starts here, so this is the one place a
     * position that arrived mid-character (a vertical move, an undo frame, a
     * click, a search hit) is made safe to edit from. */
    const char *ls = editor_line_start(fb, fb->cursor_line);
    if (ls) fb->cursor_col = editor_snap_boundary(ls, fb->cursor_col, line_len);
}

void fb_editor_toggle(FileBrowser *fb) {
    if (fb->view_mode == FVIEW_NONE) return;
    fb->sel_on = false;
    fb->editor_mode = !fb->editor_mode;
    if (fb->editor_mode) {
        fb->cursor_line = 0;
        fb->cursor_col = 0;
    }
}

/* Ensure view_content holds at least `need` bytes, growing geometrically so a
 * run of keystrokes doesn't realloc-and-copy the whole buffer each time
 * (O(n^2) → amortized O(1) reallocs). Keeps view_cap == the live allocation. */
static bool editor_ensure_cap(FileBrowser *fb, usize need) {
    /* Cursor columns and line counts are i32 throughout the editor, so the
     * payload has to stay below INT32_MAX for an all-newline document to have
     * a representable line count. That is a type limit, not a policy: a file
     * the editor refuses to OPEN past 2 MB could otherwise be grown by
     * editing to two gigabytes, and undo mirrors it twice (base + tip).
     *
     * FB_EDIT_MAX_BYTES is the policy: 32x the open cap, which leaves ample
     * room for pasting into or generating a large file, and bounds the editor
     * at 3 x 64 MiB with its two undo mirrors. */
    const usize max_payload = FB_EDIT_MAX_BYTES;
    const usize max_cap = max_payload + 1u; /* trailing NUL */
    if (!fb || need > max_cap) return false;
    if (need <= fb->view_cap) return true;
    usize cap = fb->view_cap ? fb->view_cap : 64;
    if (cap > max_cap) cap = max_cap;
    while (cap < need) {
        if (cap > max_cap / 2u) {
            cap = max_cap;
            break;
        }
        cap *= 2u;
    }
    char *b = realloc(fb->view_content, cap);
    if (!b) return false;
    /* Same document, moved: the anchor's offsets are still right, only the
     * pointer it is keyed on changed. Re-key it rather than dropping it. */
    if (fb->editor_line_cache_valid && fb->editor_line_cache_buffer == fb->view_content)
        fb->editor_line_cache_buffer = b;
    fb->view_content = b;
    fb->view_cap = cap;
    return true;
}

/* UTF-8-aware cursor stepping. cursor_col is a BYTE offset within the line, so
 * naive +-1 movement and single-byte delete land INSIDE a multi-byte sequence
 * and corrupt non-ASCII text (Turkish ö/ş/ğ, CJK, emoji, …). These step a whole
 * codepoint. */
static i32 editor_cp_len_at(const char *line, i32 col, i32 line_len) {
    /* The cell walk's own step (ui/editor_text.h): what is ONE cell on screen
     * is one step of the cursor, so an overlong or surrogate form that the
     * painter draws as two or three cells is two or three steps, not one
     * character. */
    return editor_char_len(line, line_len, col);
}
static i32 editor_cp_back_at(const char *line, i32 col) {
    /* The same step, backwards: a run of stray continuation bytes takes as
     * many steps back as it does forward, and the valid character before
     * them is a step of its own. */
    return editor_char_back(line, col, col);
}

static void editor_hfollow(FileBrowser *fb);          /* defined with fb_editor_move below */
static void editor_scroll_to_cursor(FileBrowser *fb); /* defined near the nav helpers below */

/* A keystroke's character is never a control character in the document. On
 * X11 every Ctrl+letter also arrives as its control code (Ctrl+S as U+0013,
 * for the terminal's sake), and an editor chord that ran — save, undo, paste —
 * left that code behind in the text: an invisible cell, the document modified
 * again right after the save, and the code written to disk by the next one. */
static bool editor_typed_char_ok(u32 cp) {
    return cp >= 0x20 && cp != 0x7f && !(cp >= 0x80 && cp < 0xa0) && cp <= 0x10ffff;
}

/* =========================================================================
 * The editor's selection
 *
 * The caret is the selection's moving end (cursor_line, cursor_col) and the
 * anchor (sel_line, sel_col) the fixed one, both as line and BYTE column; no
 * offsets are kept, so nothing has to move them when a line above changes
 * length — and any change of the text drops the selection (editor_apply).
 * `sel_head_*` is where the caret stood when the selection was last set: a
 * caret moved by anyone else (Find, a click, an undo, autocomplete) no longer
 * matches it, and the selection is gone, so no code that sets the caret has to
 * know there is a selection to clear.
 *
 * Nothing here is drawn. The code editor and the Markdown Edit view paint it
 * behind their glyphs (fb_render_viewer); copy, cut, typing over it and the
 * formatting keys read it from here.
 * ========================================================================= */

/* The offset of (line, col) in view_content. */
static usize ed_off(FileBrowser *fb, i32 line, i32 col) {
    char *ls = editor_line_start(fb, line);
    if (!ls) return 0;
    usize off = (usize)(ls - fb->view_content) + (usize)(col > 0 ? col : 0);
    return off > fb->view_size ? fb->view_size : off;
}

/* A selection that is still the one made, and not empty. */
static bool ed_sel_valid(const FileBrowser *fb) {
    return fb && fb->sel_on &&
           fb->sel_head_line == fb->cursor_line && fb->sel_head_col == fb->cursor_col &&
           (fb->sel_line != fb->cursor_line || fb->sel_col != fb->cursor_col);
}

/* Its two ends in document order. */
static void ed_sel_order(const FileBrowser *fb, i32 *l0, i32 *c0, i32 *l1, i32 *c1) {
    bool anchor_first = fb->sel_line < fb->cursor_line ||
                        (fb->sel_line == fb->cursor_line && fb->sel_col <= fb->cursor_col);
    if (anchor_first) {
        *l0 = fb->sel_line;    *c0 = fb->sel_col;
        *l1 = fb->cursor_line; *c1 = fb->cursor_col;
    } else {
        *l0 = fb->cursor_line; *c0 = fb->cursor_col;
        *l1 = fb->sel_line;    *c1 = fb->sel_col;
    }
}

/* Anchor at (line, col); the caret is where it is. */
static void ed_sel_set(FileBrowser *fb, i32 line, i32 col) {
    fb->sel_on = true;
    fb->sel_line = line;
    fb->sel_col = col;
    fb->sel_head_line = fb->cursor_line;
    fb->sel_head_col = fb->cursor_col;
}

/* The caret moved on its own: the selection moves with it. */
static void ed_sel_sync(FileBrowser *fb) {
    fb->sel_head_line = fb->cursor_line;
    fb->sel_head_col = fb->cursor_col;
}

bool fb_editor_sel_active(const FileBrowser *fb) {
    return fb && fb->view_content && fb->editor_mode && ed_sel_valid(fb);
}

void fb_editor_sel_clear(FileBrowser *fb) {
    if (fb) fb->sel_on = false;
}

bool fb_editor_sel_range(FileBrowser *fb, usize *from, usize *to) {
    if (!fb || !fb->view_content || !ed_sel_valid(fb)) return false;
    i32 l0, c0, l1, c1;
    ed_sel_order(fb, &l0, &c0, &l1, &c1);
    usize a = ed_off(fb, l0, c0), b = ed_off(fb, l1, c1);
    if (b <= a) return false;
    if (from) *from = a;
    if (to) *to = b;
    return true;
}

char *fb_editor_sel_text(FileBrowser *fb, usize *len) {
    usize a, b;
    if (!fb_editor_sel_range(fb, &a, &b)) return NULL;
    char *t = malloc(b - a + 1u);
    if (!t) return NULL;
    memcpy(t, fb->view_content + a, b - a);
    t[b - a] = '\0';
    if (len) *len = b - a;
    return t;
}

/* The selection removed, the caret at where it began. It is its own undo unit
 * (what was typed before is committed first), and what is typed, pasted or
 * deleted next is adjacent and joins it: "type over a selection" is one Cmd+Z. */
static bool editor_delete_selection(FileBrowser *fb) {
    if (!fb) return false;
    if (!ed_sel_valid(fb)) { fb->sel_on = false; return false; }
    i32 l0, c0, l1, c1;
    ed_sel_order(fb, &l0, &c0, &l1, &c1);
    usize a = ed_off(fb, l0, c0), b = ed_off(fb, l1, c1);
    fb->sel_on = false;
    if (b <= a) return false;
    fb_editor_push_undo(fb);
    if (!editor_apply(fb, a, b - a, NULL, 0)) return false;
    fb->cursor_line = l0;
    fb->cursor_col = c0;
    editor_clamp_cursor(fb);
    return true;
}

void fb_editor_select_all(FileBrowser *fb) {
    if (!fb || !fb->editor_mode || !fb->view_content) return;
    i32 last = fb->view_line_count > 0 ? fb->view_line_count - 1 : 0;
    fb->cursor_line = last;
    fb->cursor_col = editor_line_len(fb, last);
    ed_sel_set(fb, 0, 0);
}

/* What a character is to a double click: blank, a word (letters, digits, "_",
 * anything not ASCII — so a multi-byte character is never split), or a mark. */
static i32 ed_class(unsigned char c) {
    return (c == ' ' || c == '\t') ? 0 : (isalnum(c) || c == '_' || c >= 0x80) ? 1 : 2;
}

/* The run of one class around `col` in `line`, as [c0, c1). At the end of a line
 * it is the run before the caret. */
static void ed_word_span(FileBrowser *fb, i32 line, i32 col, i32 *c0, i32 *c1) {
    const char *ls = editor_line_start(fb, line);
    i32 len = editor_line_len(fb, line);
    if (!ls || len == 0) { *c0 = *c1 = 0; return; }
    i32 at = col < 0 ? 0 : col >= len ? len - 1 : col;
    i32 cl = ed_class((unsigned char)ls[at]);
    i32 a = at, b = at + 1;
    while (a > 0 && ed_class((unsigned char)ls[a - 1]) == cl) a--;
    while (b < len && ed_class((unsigned char)ls[b]) == cl) b++;
    *c0 = a;
    *c1 = b;
}

/* A selection unit at a position: 2 the word, 3 the line with its break (the
 * line's own end on the last). */
static void ed_unit_span(FileBrowser *fb, i32 unit, i32 line, i32 col,
                         i32 *l0, i32 *c0, i32 *l1, i32 *c1) {
    if (unit >= 3) {
        *l0 = line; *c0 = 0;
        if (line + 1 < fb->view_line_count) { *l1 = line + 1; *c1 = 0; }
        else { *l1 = line; *c1 = editor_line_len(fb, line); }
        return;
    }
    i32 a, b;
    ed_word_span(fb, line, col, &a, &b);
    *l0 = *l1 = line;
    *c0 = a;
    *c1 = b;
}

static void ed_select_unit(FileBrowser *fb, i32 unit) {
    i32 l0, c0, l1, c1;
    ed_unit_span(fb, unit, fb->cursor_line, fb->cursor_col, &l0, &c0, &l1, &c1);
    fb->cursor_line = l1;
    fb->cursor_col = c1;
    ed_sel_set(fb, l0, c0);
    fb->sel_unit = unit;
    fb->sel_unit_l0 = l0; fb->sel_unit_c0 = c0;
    fb->sel_unit_l1 = l1; fb->sel_unit_c1 = c1;
}

void fb_editor_select_word(FileBrowser *fb) {
    if (!fb || !fb->editor_mode || !fb->view_content) return;
    editor_clamp_cursor(fb);
    ed_select_unit(fb, 2);
}

void fb_editor_select_line(FileBrowser *fb) {
    if (!fb || !fb->editor_mode || !fb->view_content) return;
    editor_clamp_cursor(fb);
    ed_select_unit(fb, 3);
}

bool fb_editor_copy_selection(FileBrowser *fb) {
    if (!fb || !fb->editor_mode) return false;
    char *t = fb_editor_sel_text(fb, NULL);
    if (!t) return false;
    (void)platform_clipboard_set(t);
    free(t);
    return true;
}

bool fb_editor_cut_selection(FileBrowser *fb) {
    if (!fb || !fb->editor_mode) return false;
    char *t = fb_editor_sel_text(fb, NULL);
    if (!t) return false;
    /* Deleted only when the clipboard took it: a refused write must not cost
     * the text (the same rule as fb_editor_cut_line). */
    bool copied = platform_clipboard_set(t) == PLATFORM_CLIP_OK;
    free(t);
    if (copied && editor_delete_selection(fb)) fb_editor_push_undo(fb);
    return true;
}

/* ---- Motion ------------------------------------------------------------- */

void fb_editor_motion(FileBrowser *fb, FbMotion m, bool extend) {
    if (!fb || !fb->editor_mode || !fb->view_content) return;
    editor_clamp_cursor(fb);
    bool had = ed_sel_valid(fb);

    /* Left and Right over a selection that is not being extended collapse it to
     * the edge they point at, and go no further — as in every text field. */
    if (!extend && had && (m == FB_MOVE_LEFT || m == FB_MOVE_RIGHT)) {
        i32 l0, c0, l1, c1;
        ed_sel_order(fb, &l0, &c0, &l1, &c1);
        fb->cursor_line = m == FB_MOVE_LEFT ? l0 : l1;
        fb->cursor_col  = m == FB_MOVE_LEFT ? c0 : c1;
        fb->sel_on = false;
        fb->want_valid = false;
        editor_scroll_to_cursor(fb);
        return;
    }

    i32 al = fb->cursor_line, ac = fb->cursor_col;      /* the fixed end of a new selection */
    if (extend && had) { al = fb->sel_line; ac = fb->sel_col; }

    if (fb_md_edit_active(fb) && fb->live_width > 0 &&
        (m == FB_MOVE_UP || m == FB_MOVE_DOWN || m == FB_MOVE_PAGE_UP || m == FB_MOVE_PAGE_DOWN)) {
        f32 cy = 0, lh = fb->live_ch;
        fb_live_layout(fb, false, 0, 0, &cy, &lh);
        f32 delta = (m == FB_MOVE_PAGE_UP || m == FB_MOVE_PAGE_DOWN)
                        ? fmaxf(lh, fb->ed_area_h - lh) : lh;
        bool up = m == FB_MOVE_UP || m == FB_MOVE_PAGE_UP;
        f32 target = up ? (m == FB_MOVE_UP ? cy - 0.5f : cy - delta + 0.5f)
                        : cy + delta + 0.5f;
        f32 cx = fb->live_cursor_x;
        if (fb->want_valid && fb->want_line == fb->cursor_line && fb->want_col == fb->cursor_col)
            cx = fb->md_origin_x + fb->want_vcol * fb->live_cw;
        fb_live_layout(fb, true, cx, target, NULL, NULL);
        fb->want_valid = true;
        fb->want_vcol = (i32)roundf((cx - fb->md_origin_x) / fb->live_cw);
        fb->want_line = fb->cursor_line; fb->want_col = fb->cursor_col;
        fb->live_follow = true;
        if (extend) { fb->sel_on = true; fb->sel_line = al; fb->sel_col = ac; ed_sel_sync(fb); }
        else fb->sel_on = false;
        return;
    }

    i32 page = (fb->code_visible_lines > 1 ? fb->code_visible_lines : 21) - 1;
    switch (m) {
    case FB_MOVE_LEFT:       fb_editor_move(fb, -1, 0); break;
    case FB_MOVE_RIGHT:      fb_editor_move(fb, 1, 0); break;
    case FB_MOVE_UP:         fb_editor_move(fb, 0, -1); break;
    case FB_MOVE_DOWN:       fb_editor_move(fb, 0, 1); break;
    case FB_MOVE_WORD_LEFT:  fb_editor_move_word_left(fb); break;
    case FB_MOVE_WORD_RIGHT: fb_editor_move_word_right(fb); break;
    case FB_MOVE_LINE_START: fb_editor_goto_line_start(fb); break;
    case FB_MOVE_LINE_END:   fb_editor_goto_line_end(fb); break;
    case FB_MOVE_DOC_START:  fb_editor_goto_doc_start(fb); break;
    case FB_MOVE_DOC_END:    fb_editor_goto_doc_end(fb); break;
    case FB_MOVE_PAGE_UP:    fb_editor_move(fb, 0, -page); break;
    case FB_MOVE_PAGE_DOWN:  fb_editor_move(fb, 0, page); break;
    }

    if (extend) {
        fb->sel_on = true;
        fb->sel_line = al;
        fb->sel_col = ac;
        ed_sel_sync(fb);
    } else {
        fb->sel_on = false;
    }
}

/* ---- The pointer -------------------------------------------------------- */

/* Move the caret to a framebuffer point: through the live preview's own layout
 * in the Markdown Edit view, the code editor's cached grid otherwise. */
static bool ed_locate(FileBrowser *fb, f32 mx, f32 my) {
    if (fb->view_mode == FVIEW_MARKDOWN && fb->md_raw_mode) return fb_md_live_locate(fb, mx, my);
    return fb_editor_click_caret(fb, mx, my);
}

bool fb_editor_press(FileBrowser *fb, f32 mx, f32 my, i32 clicks, bool extend) {
    if (!fb || !fb->editor_mode || !fb->view_content) return false;
    editor_clamp_cursor(fb);
    bool had = ed_sel_valid(fb);
    i32 al = had ? fb->sel_line : fb->cursor_line;      /* Shift+click grows from here */
    i32 ac = had ? fb->sel_col : fb->cursor_col;
    if (!ed_locate(fb, mx, my)) return false;
    fb->want_valid = false;
    fb->sel_unit = 1;
    if (extend && clicks <= 1) {
        fb->sel_on = true;
        fb->sel_line = al;
        fb->sel_col = ac;
        ed_sel_sync(fb);
    } else if (clicks >= 3) {
        ed_select_unit(fb, 3);
    } else if (clicks == 2) {
        ed_select_unit(fb, 2);
    } else {
        ed_sel_set(fb, fb->cursor_line, fb->cursor_col);    /* empty: the anchor of a drag */
    }
    return true;
}

/* The free end of a drag at the caret: by the unit first picked, which stays
 * selected whichever way the pointer goes from it. */
static void ed_drag_apply(FileBrowser *fb) {
    if (fb->sel_unit >= 2) {
        i32 u0, c0, u1, c1;
        ed_unit_span(fb, fb->sel_unit, fb->cursor_line, fb->cursor_col, &u0, &c0, &u1, &c1);
        bool before = fb->cursor_line < fb->sel_unit_l0 ||
                      (fb->cursor_line == fb->sel_unit_l0 && fb->cursor_col < fb->sel_unit_c0);
        if (before) {
            fb->sel_line = fb->sel_unit_l1; fb->sel_col = fb->sel_unit_c1;
            fb->cursor_line = u0; fb->cursor_col = c0;
        } else {
            fb->sel_line = fb->sel_unit_l0; fb->sel_col = fb->sel_unit_c0;
            fb->cursor_line = u1; fb->cursor_col = c1;
        }
    }
    ed_sel_sync(fb);
}

i32 fb_editor_drag(FileBrowser *fb, f32 mx, f32 my) {
    if (!fb || !fb->editor_mode || !fb->view_content || !fb->sel_on) return 0;
    i32 dir = 0;
    f32 yy = my;
    if (fb->ed_area_h > 0.0f) {
        f32 top = fb->ed_area_y, bot = fb->ed_area_y + fb->ed_area_h;
        if (my < top) { dir = -1; yy = top + 1.0f; }
        else if (my >= bot) { dir = 1; yy = bot - 1.0f; }
    }
    if (!ed_locate(fb, mx, yy)) return dir;
    ed_drag_apply(fb);
    return dir;
}

void fb_editor_drag_scroll(FileBrowser *fb, i32 dir, f32 mx) {
    if (!fb || !dir || !fb->editor_mode || !fb->view_content || !fb->sel_on) return;
    if (fb_md_edit_active(fb)) {
        fb->view_scroll_px = fmaxf(0.0f, fminf(fb->view_scroll_px + dir * fb->live_ch,
                                           fb->view_content_px - fb->ed_area_h));
    } else if (dir < 0) { if (fb->view_scroll > 0) fb->view_scroll--; }
    else if (fb->view_scroll + 1 < fb->view_line_count) fb->view_scroll++;
    f32 yy = dir < 0 ? fb->ed_area_y + 1.0f : fb->ed_area_y + fb->ed_area_h - 1.0f;
    if (!ed_locate(fb, mx, yy)) return;
    ed_drag_apply(fb);
}

/* ---- Editing over a selection or a line --------------------------------- */

/* The lines an indent-like command acts on: the selection's, less the last
 * when the selection ends at its first column; the caret's line otherwise. */
static void ed_target_lines(FileBrowser *fb, bool *had, i32 *first, i32 *last) {
    *had = ed_sel_valid(fb);
    if (*had) {
        i32 l0, c0, l1, c1;
        ed_sel_order(fb, &l0, &c0, &l1, &c1);
        *first = l0;
        *last = (l1 > l0 && c1 == 0) ? l1 - 1 : l1;
    } else {
        *first = *last = fb->cursor_line;
    }
}

/* What to do to one line of a block: take `drop` bytes out at byte `at` of its
 * text and put `ins` there. */
typedef struct { const char *ins; usize ins_len; usize drop; usize at; } LineEdit;

/* Lines first..last rewritten by `ed[]`, as ONE splice — a block that is several
 * (even adjacent) edits is several undo frames, and Cmd+Z must take back what
 * one key did. The breaks between the lines are the document's own bytes. */
static bool ed_edit_lines(FileBrowser *fb, i32 first, i32 last, const LineEdit *ed) {
    i32 n = last - first + 1;
    usize rs = ed_off(fb, first, 0);
    usize re = ed_off(fb, last, editor_line_len(fb, last));
    if (n < 1 || re < rs) return false;
    usize cap = re - rs + 1u;
    for (i32 k = 0; k < n; k++) cap += ed[k].ins_len;
    char *buf = malloc(cap);
    if (!buf) return false;
    usize w = 0;
    for (i32 k = 0; k < n; k++) {
        i32 ln = first + k;
        const char *ls = editor_line_start(fb, ln);
        usize len = (usize)editor_line_len(fb, ln);
        usize at = ed[k].at > len ? len : ed[k].at;
        usize drop = ed[k].drop > len - at ? len - at : ed[k].drop;
        memcpy(buf + w, ls, at); w += at;
        if (ed[k].ins_len) { memcpy(buf + w, ed[k].ins, ed[k].ins_len); w += ed[k].ins_len; }
        memcpy(buf + w, ls + at + drop, len - at - drop); w += len - at - drop;
        if (ln < last) {                                   /* the break that follows, as it was */
            usize eb = (usize)editor_line_eol(fb, ln);
            memcpy(buf + w, ls + len, eb); w += eb;
        }
    }
    bool ok = editor_apply(fb, rs, re - rs, buf, w);
    free(buf);
    return ok;
}

void fb_editor_indent(FileBrowser *fb, bool outdent) {
    if (!fb || !fb->editor_mode || !fb->view_content) return;
    editor_clamp_cursor(fb);
    bool had;
    i32 first, last;
    ed_target_lines(fb, &had, &first, &last);
    i32 n = last - first + 1;
    LineEdit *ed = calloc((usize)n, sizeof *ed);
    if (!ed) return;
    i32 al = fb->sel_line, ac = fb->sel_col;
    i32 d_caret = 0, d_anchor = 0;
    bool changed = false;
    for (i32 k = 0; k < n; k++) {
        i32 ln = first + k;
        const char *ls = editor_line_start(fb, ln);
        i32 len = editor_line_len(fb, ln);
        i32 delta = 0;
        if (!outdent) {
            if (len > 0 || n == 1) {                       /* a blank line in a block stays blank */
                ed[k].ins = "    ";
                ed[k].ins_len = 4;
                delta = 4;
            }
        } else {
            i32 d = 0;
            if (len > 0 && ls[0] == '\t') d = 1;
            else while (d < 4 && d < len && ls[d] == ' ') d++;
            ed[k].drop = (usize)d;
            delta = -d;
        }
        if (delta) changed = true;
        if (ln == fb->cursor_line) d_caret = delta;
        if (had && ln == al) d_anchor = delta;
    }
    if (!changed) { free(ed); return; }
    fb_editor_push_undo(fb);
    bool ok = ed_edit_lines(fb, first, last, ed);
    free(ed);
    if (!ok) return;
    /* The ends follow their text; a selection that began at a line's first
     * column keeps beginning there, so the new indent is inside it. */
    i32 nc = fb->cursor_col + d_caret;
    if (had && d_caret > 0 && fb->cursor_col == 0) nc = 0;
    fb->cursor_col = nc < 0 ? 0 : nc;
    if (had) {
        i32 na = ac + d_anchor;
        if (d_anchor > 0 && ac == 0) na = 0;
        ed_sel_set(fb, al, na < 0 ? 0 : na);
    }
    editor_clamp_cursor(fb);
    fb_editor_push_undo(fb);
    editor_hfollow(fb);
}

/* Tab: over several lines it indents them; on a Markdown list item it indents
 * the item; anywhere else it is four spaces (over a selection, instead of it).
 * Shift+Tab takes an indent off the line or lines. */
void fb_editor_tab(FileBrowser *fb, bool back) {
    if (!fb || !fb->editor_mode || !fb->view_content) return;
    editor_clamp_cursor(fb);
    if (back) { fb_editor_indent(fb, true); return; }
    bool had;
    i32 first, last;
    ed_target_lines(fb, &had, &first, &last);
    if ((had && last > first) || (!had && fb_md_edit_active(fb) && fb_editor_md_list_line(fb))) {
        fb_editor_indent(fb, false);
        return;
    }
    for (i32 i = 0; i < 4; i++) fb_editor_insert_char(fb, ' ');
}

bool fb_editor_md_list_line(FileBrowser *fb) {
    if (!fb || !fb->view_content) return false;
    editor_clamp_cursor(fb);
    const char *ls = editor_line_start(fb, fb->cursor_line);
    i32 ll = editor_line_len(fb, fb->cursor_line);
    if (!ls) return false;
    i32 i = 0;
    while (i < ll && (ls[i] == ' ' || ls[i] == '\t')) i++;
    if (i + 1 < ll && (ls[i] == '-' || ls[i] == '*' || ls[i] == '+') && ls[i + 1] == ' ') return true;
    i32 k = i;
    while (k < ll && isdigit((unsigned char)ls[k])) k++;
    return k > i && k + 1 < ll && ls[k] == '.' && ls[k + 1] == ' ';
}

/* Option+Up / Down: the selected lines (or the caret's) trade places with the
 * line above or below. One splice, one undo unit; the selection goes with them. */
void fb_editor_move_lines(FileBrowser *fb, i32 dir) {
    if (!fb || !fb->editor_mode || !fb->view_content || (dir != -1 && dir != 1)) return;
    editor_clamp_cursor(fb);
    bool had;
    i32 first, last;
    ed_target_lines(fb, &had, &first, &last);
    i32 n = dir < 0 ? first - 1 : last + 1;
    if (n < 0 || n >= fb->view_line_count) return;
    i32 top = dir < 0 ? n : first, bot = dir < 0 ? last : n;
    usize rs = ed_off(fb, top, 0), re = ed_off(fb, bot, editor_line_len(fb, bot));
    usize bs = ed_off(fb, first, 0), be = ed_off(fb, last, editor_line_len(fb, last));
    usize ns = ed_off(fb, n, 0), ne = ed_off(fb, n, editor_line_len(fb, n));
    if (re < rs || be < bs || ne < ns) return;
    /* Between the two parts goes the file's own break. */
    const char *eol = fb->view_crlf ? "\r\n" : "\n";
    usize elen = strlen(eol), blen = be - bs, nlen = ne - ns;
    char *buf = malloc(blen + elen + nlen + 1u);
    if (!buf) return;
    if (dir < 0) {
        memcpy(buf, fb->view_content + bs, blen);
        memcpy(buf + blen, eol, elen);
        memcpy(buf + blen + elen, fb->view_content + ns, nlen);
    } else {
        memcpy(buf, fb->view_content + ns, nlen);
        memcpy(buf + nlen, eol, elen);
        memcpy(buf + nlen + elen, fb->view_content + bs, blen);
    }
    i32 al = fb->sel_line, ac = fb->sel_col;
    fb_editor_push_undo(fb);
    bool ok = editor_apply(fb, rs, re - rs, buf, blen + elen + nlen);
    free(buf);
    if (!ok) return;
    fb->cursor_line += dir;
    if (had) ed_sel_set(fb, al + dir, ac);
    editor_clamp_cursor(fb);
    fb_editor_push_undo(fb);
    editor_scroll_to_cursor(fb);
}

/* Cmd+Enter / Cmd+Shift+Enter: a new line below or above, indented like the
 * caret's, with the caret in it. */
void fb_editor_open_line(FileBrowser *fb, bool above) {
    if (!fb || !fb->editor_mode || !fb->view_content) return;
    editor_clamp_cursor(fb);
    char indent[256];
    const char *ls = editor_line_start(fb, fb->cursor_line);
    i32 len = editor_line_len(fb, fb->cursor_line);
    i32 ind = 0;
    while (ind < len && ind < (i32)sizeof indent - 2 && (ls[ind] == ' ' || ls[ind] == '\t')) {
        indent[ind] = ls[ind];
        ind++;
    }
    const char *eol = fb->view_crlf ? "\r\n" : "\n";
    usize elen = strlen(eol);
    char ins[256 + 4];
    usize n = 0;
    usize at;
    if (above) {
        memcpy(ins, indent, (usize)ind); n = (usize)ind;
        memcpy(ins + n, eol, elen); n += elen;
        at = ed_off(fb, fb->cursor_line, 0);
    } else {
        memcpy(ins, eol, elen); n = elen;
        memcpy(ins + n, indent, (usize)ind); n += (usize)ind;
        at = ed_off(fb, fb->cursor_line, len);
    }
    fb_editor_push_undo(fb);
    if (!editor_apply(fb, at, 0, ins, n)) return;
    if (!above) fb->cursor_line++;
    fb->cursor_col = ind;
    editor_clamp_cursor(fb);
    fb_editor_push_undo(fb);
    editor_scroll_to_cursor(fb);
}

/* Cmd+/: comment the lines out, or back in. A language's own line comment
 * (syntax.c's table); in a Markdown note an HTML comment round the block. The
 * selection (or the caret) stays where its text goes. One undo unit. */
void fb_editor_toggle_comment(FileBrowser *fb) {
    if (!fb || !fb->editor_mode || !fb->view_content) return;
    editor_clamp_cursor(fb);
    bool had;
    i32 first, last;
    ed_target_lines(fb, &had, &first, &last);
    bool md = fb->view_mode == FVIEW_MARKDOWN;
    const char *tok = md ? NULL : syntax_line_comment(fb->code_lang);
    if (!md && (!tok || !tok[0])) return;
    usize tlen = tok ? strlen(tok) : 0;
    bool caret_last = fb->cursor_line > fb->sel_line ||
                      (fb->cursor_line == fb->sel_line && fb->cursor_col >= fb->sel_col);

    if (md) {
        const char *fl = editor_line_start(fb, first);
        i32 fn = editor_line_len(fb, first), fi = 0;
        while (fi < fn && (fl[fi] == ' ' || fl[fi] == '\t')) fi++;
        const char *ll = editor_line_start(fb, last);
        i32 ln = editor_line_len(fb, last);
        usize s_at = ed_off(fb, first, fi), e_at = ed_off(fb, last, ln);
        if (e_at < s_at) return;
        usize span = e_at - s_at;
        bool off = span >= 7 && memcmp(fb->view_content + s_at, "<!--", 4) == 0 &&
                   memcmp(fb->view_content + e_at - 3, "-->", 3) == 0;
        (void)ll;
        usize skip_open = off && fb->view_content[s_at + 4] == ' ' ? 5u : 4u;
        usize skip_close = off && span >= skip_open + 4 && fb->view_content[e_at - 4] == ' ' ? 4u : 3u;
        char *buf = malloc(span + 16u);
        if (!buf) return;
        usize w = 0;
        if (off) {
            if (span < skip_open + skip_close) { free(buf); return; }
            memcpy(buf, fb->view_content + s_at + skip_open, span - skip_open - skip_close);
            w = span - skip_open - skip_close;
        } else {
            memcpy(buf, "<!-- ", 5); w = 5;
            memcpy(buf + w, fb->view_content + s_at, span); w += span;
            memcpy(buf + w, " -->", 4); w += 4;
        }
        fb_editor_push_undo(fb);
        bool ok = editor_apply(fb, s_at, span, buf, w);
        free(buf);
        if (!ok) return;
        /* Select the lines it acted on, in the direction the selection had. */
        i32 end_col = editor_line_len(fb, last);
        if (had) {
            fb->cursor_line = caret_last ? last : first;
            fb->cursor_col = caret_last ? end_col : 0;
            ed_sel_set(fb, caret_last ? first : last, caret_last ? 0 : end_col);
        } else {
            fb->cursor_col = fb->cursor_col > end_col ? end_col : fb->cursor_col;
        }
        editor_clamp_cursor(fb);
        fb_editor_push_undo(fb);
        return;
    }

    /* Every non-blank line already commented: uncomment. Otherwise comment them,
     * all at the shallowest indent of the block. */
    i32 n = last - first + 1;
    bool all = true, any = false;
    i32 min_ind = 1 << 20;
    for (i32 k = 0; k < n; k++) {
        const char *ls = editor_line_start(fb, first + k);
        i32 len = editor_line_len(fb, first + k), i = 0;
        while (i < len && (ls[i] == ' ' || ls[i] == '\t')) i++;
        if (i >= len) continue;                        /* blank */
        any = true;
        if (i < min_ind) min_ind = i;
        if (!((usize)(len - i) >= tlen && memcmp(ls + i, tok, tlen) == 0)) all = false;
    }
    if (!any) return;
    LineEdit *ed = calloc((usize)n, sizeof *ed);
    if (!ed) return;
    char ins[40];
    snprintf(ins, sizeof ins, "%s ", tok);
    usize ilen = strlen(ins);
    i32 al = fb->sel_line, ac = fb->sel_col, d_caret = 0, d_anchor = 0;
    for (i32 k = 0; k < n; k++) {
        i32 ln = first + k;
        const char *ls = editor_line_start(fb, ln);
        i32 len = editor_line_len(fb, ln), i = 0;
        while (i < len && (ls[i] == ' ' || ls[i] == '\t')) i++;
        if (i >= len) continue;
        i32 delta;
        if (all) {
            usize d = tlen + ((usize)(len - i) > tlen && ls[i + (i32)tlen] == ' ' ? 1u : 0u);
            ed[k].at = (usize)i;
            ed[k].drop = d;
            delta = -(i32)d;
        } else {
            ed[k].at = (usize)min_ind;
            ed[k].ins = ins;
            ed[k].ins_len = ilen;
            delta = (i32)ilen;
        }
        if (ln == fb->cursor_line) d_caret = delta;
        if (had && ln == al) d_anchor = delta;
    }
    fb_editor_push_undo(fb);
    bool ok = ed_edit_lines(fb, first, last, ed);
    free(ed);
    if (!ok) return;
    i32 nc = fb->cursor_col + d_caret;
    fb->cursor_col = nc < 0 ? 0 : nc;
    if (had) { i32 na = ac + d_anchor; ed_sel_set(fb, al, na < 0 ? 0 : na); }
    editor_clamp_cursor(fb);
    fb_editor_push_undo(fb);
}

/* Ctrl+K: to the end of the line; at its end, the break (the next line joins). */
void fb_editor_kill_to_line_end(FileBrowser *fb) {
    if (!fb || !fb->editor_mode || !fb->view_content) return;
    editor_clamp_cursor(fb);
    if (editor_delete_selection(fb)) return;
    i32 len = editor_line_len(fb, fb->cursor_line);
    if (fb->cursor_col >= len) { fb_editor_delete(fb); return; }
    editor_pre_edit(fb);
    usize off = ed_off(fb, fb->cursor_line, fb->cursor_col);
    (void)editor_apply(fb, off, (usize)(len - fb->cursor_col), NULL, 0);
}

void fb_editor_insert_char(FileBrowser *fb, u32 cp) {
    if (!fb->editor_mode || !fb->view_content) return;
    if (!editor_typed_char_ok(cp)) return;
    editor_clamp_cursor(fb);
    editor_pre_edit(fb);
    (void)editor_delete_selection(fb);       /* typed over a selection: it goes */

    char *line_start = editor_line_start(fb, fb->cursor_line);
    if (!line_start) return;
    u8 utf[4];
    u32 enc_len = utf8_encode(cp, utf);
    if (enc_len == 0) return;
    usize offset = (usize)(line_start - fb->view_content) + (usize)fb->cursor_col;
    if (offset > fb->view_size) return;

    if (!editor_apply(fb, offset, 0, (const char *)utf, enc_len)) return;
    fb->cursor_col += (i32)enc_len;
    editor_hfollow(fb);
}

void fb_editor_backspace(FileBrowser *fb) {
    if (!fb->editor_mode || !fb->view_content) return;
    editor_clamp_cursor(fb);
    editor_pre_edit(fb);
    if (editor_delete_selection(fb)) { editor_hfollow(fb); return; }

    if (fb->cursor_col > 0) {
        char *line_start = editor_line_start(fb, fb->cursor_line);
        i32 back = editor_cp_back_at(line_start, fb->cursor_col);
        if (back < 1) back = 1;
        usize gc = (usize)(line_start - fb->view_content) + (usize)fb->cursor_col;
        if (!editor_apply(fb, gc - (usize)back, (usize)back, NULL, 0)) return;
        fb->cursor_col -= back;
    } else if (fb->cursor_line > 0) {
        /* Join with previous line: its whole break goes, CR and LF alike. */
        i32 prev_len = editor_line_len(fb, fb->cursor_line - 1);
        usize eol = (usize)editor_line_eol(fb, fb->cursor_line - 1);
        char *line_start = editor_line_start(fb, fb->cursor_line);
        usize at = (usize)(line_start - fb->view_content);
        if (eol == 0 || eol > at) return;
        if (!editor_apply(fb, at - eol, eol, NULL, 0)) return;
        fb->cursor_line--;
        fb->cursor_col = prev_len;
    }
    editor_hfollow(fb);
}

void fb_editor_delete(FileBrowser *fb) {
    if (!fb->editor_mode || !fb->view_content) return;
    editor_clamp_cursor(fb);
    editor_pre_edit(fb);
    if (editor_delete_selection(fb)) { editor_hfollow(fb); return; }

    char *line_start = editor_line_start(fb, fb->cursor_line);
    i32 line_len = editor_line_len(fb, fb->cursor_line);

    if (fb->cursor_col < line_len) {
        i32 dl = editor_cp_len_at(line_start, fb->cursor_col, line_len);
        if (dl < 1) dl = 1;
        usize gc = (usize)(line_start - fb->view_content) + (usize)fb->cursor_col;
        editor_apply(fb, gc, (usize)dl, NULL, 0);
    } else if (fb->cursor_line < fb->view_line_count - 1) {
        /* Join with next line — delete this line's break, both bytes of a CRLF */
        usize offset = (usize)(line_start - fb->view_content) + (usize)line_len;
        usize eol = (usize)editor_line_eol(fb, fb->cursor_line);
        if (eol) editor_apply(fb, offset, eol, NULL, 0);
    }
}

void fb_editor_newline(FileBrowser *fb) {
    if (!fb->editor_mode || !fb->view_content) return;
    editor_clamp_cursor(fb);
    editor_pre_edit(fb);
    (void)editor_delete_selection(fb);

    char *line_start = editor_line_start(fb, fb->cursor_line);
    usize offset = (usize)(line_start - fb->view_content) + (usize)fb->cursor_col;
    /* The FILE's line ending: a bare LF typed into a CRLF file left it with
     * two kinds of line, which is what git and the next tool to read it
     * report as a changed file. */
    const char *brk = fb->view_crlf ? "\r\n" : "\n";
    if (!editor_apply(fb, offset, 0, brk, strlen(brk))) return;
    fb->cursor_line++;
    fb->cursor_col = 0;
    editor_hfollow(fb);
}

/* Enter inside a markdown list: continue the marker ("- ", "* ", "N. ",
 * "- [ ] "); an empty item instead clears the marker. */
void fb_md_editor_newline(FileBrowser *fb) {
    if (!fb || !fb->editor_mode || !fb->view_content) { if (fb) fb_editor_newline(fb); return; }
    editor_clamp_cursor(fb);
    (void)editor_delete_selection(fb);     /* the line it continues is the one the selection left */
    const char *ls = editor_line_start(fb, fb->cursor_line);
    i32 ll = editor_line_len(fb, fb->cursor_line);
    i32 ind = 0; while (ind < ll && (ls[ind]==' '||ls[ind]=='\t')) ind++;
    const char *t = ls + ind; i32 tl = ll - ind;
    i32 mlen = 0, is_task = 0, onum = 0; char bm = 0;
    if (tl>=5 && (t[0]=='-'||t[0]=='*'||t[0]=='+') && t[1]==' ' && t[2]=='[' &&
        (t[3]==' '||t[3]=='x'||t[3]=='X') && t[4]==']') { mlen=ind+6; is_task=1; }
    else if (tl>=2 && (t[0]=='-'||t[0]=='*'||t[0]=='+') && t[1]==' ') { mlen=ind+2; bm=t[0]; }
    else {
        i32 k = 0;
        while (k < tl && isdigit((unsigned char)t[k])) k++;
        if (k > 0 && k + 1 < tl && t[k] == '.' && t[k + 1] == ' ') {
            mlen = ind + k + 2;
            for (i32 j = 0; j < k; j++) {
                i32 digit = t[j] - '0';
                if (onum > (INT32_MAX - digit) / 10) {
                    onum = INT32_MAX;
                    break;
                }
                onum = onum * 10 + digit;
            }
        }
    }
    if (mlen == 0) { fb_editor_newline(fb); return; }

    bool empty = true;
    for (i32 k = mlen; k < ll; k++) if (ls[k] != ' ' && ls[k] != '\t') { empty = false; break; }
    if (empty) {                              /* blank item → drop the marker */
        fb->cursor_col = ll;
        while (fb->cursor_col > ind) fb_editor_backspace(fb);
        return;
    }
    fb_editor_newline(fb);
    for (i32 k = 0; k < ind; k++) fb_editor_insert_char(fb, ' ');
    if (is_task)      { const char *m = "- [ ] "; for (const char *c=m; *c; c++) fb_editor_insert_char(fb, (u32)*c); }
    else if (onum>0)  { char b[16]; i32 next = onum < INT32_MAX ? onum + 1 : INT32_MAX; snprintf(b, sizeof b, "%d. ", next); for (char *c=b; *c; c++) fb_editor_insert_char(fb, (u32)*c); }
    else              { fb_editor_insert_char(fb, (u32)(bm?bm:'-')); fb_editor_insert_char(fb, ' '); }
}

/* Map a click in the live-preview body (framebuffer coords) to a (line, col)
 * and move the cursor there. Replays the per-line heights from the last render
 * so variable heading heights line up. */
bool fb_md_live_locate(FileBrowser *fb, f32 mx, f32 my) {
    if (!fb || fb->view_mode != FVIEW_MARKDOWN || !fb->md_raw_mode || !fb->view_content) return false;
    if (fb->live_cw <= 0.0f) return false;
    fb_live_layout(fb, true, mx, my - fb->md_origin_y + fb->view_scroll_px, NULL, NULL);
    editor_clamp_cursor(fb);
    return true;
}

/* How wide this view draws a tab. The code and text editor runs it to the next
 * stop; the Markdown live editor draws one cell for it (fb_live_inline_raw),
 * and the column arithmetic has to be the painter's or the caret leaves the
 * glyphs. */
static i32 editor_tab_cols(const FileBrowser *fb) {
    return fb->view_mode == FVIEW_MARKDOWN ? 1 : FB_TAB_COLS;
}

/* Visual column of the cursor — cursor_col is a byte offset. The sum of the
 * cells before it (ui/editor_text.h), which is where the caret is drawn. */
static i32 editor_cursor_vcol(FileBrowser *fb) {
    char *ls = editor_line_start(fb, fb->cursor_line);
    if (!ls) return 0;
    i32 ll = editor_line_len(fb, fb->cursor_line);
    i32 cc = fb->cursor_col;
    if (cc < 0) cc = 0;
    if (cc > ll) cc = ll;
    return editor_col_at(ls, ll, cc, editor_tab_cols(fb));
}

/* Keep the caret horizontally visible: follow view_scroll_x the same way the
 * vertical view_scroll follows cursor_line. code_visible_cols comes from the
 * last render; before the first render fall back to a safe width. */
static void editor_hfollow(FileBrowser *fb) {
    if (fb->view_mode == FVIEW_MARKDOWN && fb->md_raw_mode) {
        fb->live_follow = true;
        MdLiveFence fs;
        const char *p = md_live_walk_to(fb->view_content, fb->view_size, fb->cursor_line, &fs, NULL);
        i32 len = editor_line_len(fb, fb->cursor_line);
        MdLiveLine cls = md_live_classify(p, len, &fs);
        if (!cls.code && !cls.fence) return;
    }
    i32 vis = fb->code_visible_cols > 0 ? fb->code_visible_cols : 80;
    i32 vcol = editor_cursor_vcol(fb);
    if (vcol < fb->view_scroll_x) fb->view_scroll_x = vcol;
    if (vcol >= fb->view_scroll_x + vis) fb->view_scroll_x = vcol - vis + 1;
    if (fb->view_scroll_x < 0) fb->view_scroll_x = 0;
}

/* Byte offset of the cell boundary nearest to visual column `vcol` of a line
 * (its end when the line is shorter). The columns are the painter's — a
 * malformed byte one of its own, a tab as wide as it is drawn — so "the same
 * column" on two lines is the same place on screen. Always a character
 * boundary. */
static i32 editor_byte_for_vcol(const char *line, i32 line_len, i32 vcol, i32 tab) {
    return editor_snap_boundary(line, editor_byte_at_col(line, line_len, vcol, tab), line_len);
}

void fb_editor_move(FileBrowser *fb, i32 dcol, i32 drow) {
    if (!fb || !fb->editor_mode || !fb->view_content) return;

    if (fb->view_line_count < 1) fb->view_line_count = 1;

    /* The column a vertical move keeps: the remembered one while the cursor is
     * still where the last vertical move left it, otherwise the column it is in
     * now. Read before the line changes. */
    i32 want = (fb->want_valid && fb->want_line == fb->cursor_line &&
                fb->want_col == fb->cursor_col)
                   ? fb->want_vcol : editor_cursor_vcol(fb);

    i64 target_line = (i64)fb->cursor_line + (i64)drow;
    if (target_line < 0) target_line = 0;
    if (target_line >= fb->view_line_count) target_line = fb->view_line_count - 1;
    fb->cursor_line = (i32)target_line;

    i32 line_len = editor_line_len(fb, fb->cursor_line);
    char *line_start = editor_line_start(fb, fb->cursor_line);

    /* Vertical motion arrives at the user's COLUMN, not at the byte offset the
     * previous line happened to have: those are different places as soon as one
     * of the two lines holds a multi-byte character, and the byte offset can
     * fall in the middle of one. */
    if (drow != 0 && line_start)
        fb->cursor_col = editor_byte_for_vcol(line_start, line_len, want, editor_tab_cols(fb));

    /* Horizontal motion steps whole codepoints (cursor_col is a byte offset). */
    if (dcol > 0) {
        for (i32 s = 0; s < dcol && fb->cursor_col < line_len; s++)
            fb->cursor_col += editor_cp_len_at(line_start, fb->cursor_col, line_len);
    } else if (dcol < 0) {
        i64 steps = -(i64)dcol; /* INT32_MIN-safe */
        for (i64 s = 0; s < steps && fb->cursor_col > 0; s++)
            fb->cursor_col -= editor_cp_back_at(line_start, fb->cursor_col);
    }
    if (fb->cursor_col < 0) fb->cursor_col = 0;
    if (fb->cursor_col > line_len) fb->cursor_col = line_len;

    /* Ensure cursor is visible — against the rows the last render showed. */
    i32 visible = fb->code_visible_lines > 0 ? fb->code_visible_lines : 30;
    if (fb->cursor_line < fb->view_scroll) fb->view_scroll = fb->cursor_line;
    if (fb->cursor_line >= fb->view_scroll + visible) fb->view_scroll = fb->cursor_line - visible + 1;
    editor_clamp_cursor(fb);
    editor_hfollow(fb);

    /* Remember the column only for a purely vertical move; a horizontal one
     * makes wherever it ended the new target. */
    fb->want_valid = drow != 0 && dcol == 0;
    fb->want_vcol  = want;
    fb->want_line  = fb->cursor_line;
    fb->want_col   = fb->cursor_col;
}

/* =========================================================================
 * The open document and its file
 *
 * A save says whether it worked, checks that the file it replaces is still
 * the one that was read, and leaves a file the user locked alone unless the
 * save is repeated. The document follows its file when it is renamed or
 * trashed from the list beside it, so a later save does not fail on
 * realpath() for a name that no longer exists.
 * ========================================================================= */

/* Where a save of view_path lands: the file a link at that path points to (so
 * the rename does not replace the LINK), or — when nothing is at that path any
 * more — the same name in the same folder, which is how a file deleted under
 * the editor is written again. A link whose target is gone is not replaced
 * with a file: that fails, with ENOENT. `out` holds ATOMIC_FILE_PATH_CAP
 * bytes. */
static bool fb_save_target(const FileBrowser *fb, char *out) {
    if (realpath(fb->view_path, out)) return true;
    if (errno != ENOENT) return false;
    struct stat st;
    if (lstat(fb->view_path, &st) == 0) { errno = ENOENT; return false; }
    char dir[FB_MAX_PATH];
    snprintf(dir, sizeof dir, "%s", fb->view_path);
    char *slash = strrchr(dir, '/');
    if (!slash || !slash[1]) { errno = ENOENT; return false; }
    char base[FB_MAX_PATH];
    snprintf(base, sizeof base, "%s", slash + 1);
    if (slash == dir) slash[1] = '\0'; else *slash = '\0';
    char parent[ATOMIC_FILE_PATH_CAP];
    if (!realpath(dir, parent)) return false;
    int n = snprintf(out, ATOMIC_FILE_PATH_CAP, "%s%s%s", parent,
                     strcmp(parent, "/") ? "/" : "", base);
    if (n < 0 || n >= ATOMIC_FILE_PATH_CAP) { errno = ENAMETOOLONG; return false; }
    return true;
}

/* Write the text to its file, as a document the user owns: the replacement
 * keeps the old file's owner, extended attributes and ACL, a file with other
 * hard links is rewritten in place, and one the user made read-only is left
 * alone (`*refused_readonly`) unless `force` — the user repeated the save.
 * On success the view remembers the file as it now is. On failure errno says
 * why. */
static bool fb_write_view_atomic(FileBrowser *fb, bool force, bool *refused_readonly) {
    if (refused_readonly) *refused_readonly = false;
    if (!fb || !fb->view_content || fb->source != FB_SOURCE_LOCAL ||
        !fb->view_path[0]) { errno = EINVAL; return false; }

    char resolved[ATOMIC_FILE_PATH_CAP];
    if (!fb_save_target(fb, resolved)) return false;

    AtomicFileWriter writer;
    if (!atomic_file_begin(&writer, resolved)) return false;
    atomic_file_keep(&writer, force ? (ATOMIC_KEEP_DOCUMENT & ~ATOMIC_KEEP_READONLY)
                                    : ATOMIC_KEEP_DOCUMENT);
    FILE *f = writer.stream;
    usize off = 0;
    while (off < fb->view_size) {
        usize n = fwrite(fb->view_content + off, 1, fb->view_size - off, f);
        if (n == 0) {
            int why = errno;
            atomic_file_abort(&writer);
            errno = why;
            return false;
        }
        off += n;
    }
    if (!atomic_file_commit(&writer)) {
        if (refused_readonly) *refused_readonly = writer.refused_readonly;
        return false;
    }
    /* The identity of the bytes just written, off the descriptor that wrote
     * them: the next save compares the disk with THIS. */
    fb->view_disk = writer.committed_valid ? file_identity_of(&writer.committed_st)
                                           : (FileIdentity){0};
    return true;
}

/* Save, and say what happened. The text is not written over a file that is no
 * longer the one it came from, nor over one the user made read-only, until
 * the save is REPEATED (ui/unsaved_guard.h), so a file an agent rewrote on
 * disk is not lost to one Cmd+S. Every way it does not save leaves the reason
 * in the notice and in save_result. */
bool fb_editor_save(FileBrowser *fb) {
    if (!fb || !fb->editor_mode || !fb->modified || !fb->view_content)
        return false;

    f64 now = platform_time_sec();
    /* One confirmation covers both rules: a repeat is "write it anyway". */
    bool force = unsaved_guard_confirmed(&fb->guard, UNSAVED_OVERWRITE, now);
    FileDiskState disk = file_identity_check(fb->view_path, &fb->view_disk);
    if (disk == FILE_DISK_CHANGED && !force) {
        unsaved_guard_refuse(&fb->guard, UNSAVED_OVERWRITE, now);
        unsaved_guard_message(fb->notice, sizeof fb->notice, UNSAVED_OVERWRITE, fb_view_name(fb));
        fb_notice_stamp(fb, true);
        fb->save_result = FB_SAVE_REFUSED_CHANGED;
        return false;
    }

    errno = 0;
    bool readonly = false;
    if (!fb_write_view_atomic(fb, force, &readonly)) {
        int why = errno;
        if (readonly) {
            unsaved_guard_refuse(&fb->guard, UNSAVED_OVERWRITE, now);
            unsaved_guard_message_readonly(fb->notice, sizeof fb->notice, fb_view_name(fb));
            fb_notice_stamp(fb, true);
            fb->save_result = FB_SAVE_REFUSED_READONLY;
            return false;
        }
        fb->save_result = FB_SAVE_FAILED;
        fb->save_errno = why;
        fb_notice(fb, true, "Not saved: %s", why ? strerror(why) : "the file could not be written");
        return false;
    }
    fb->modified = false;
    fb->save_result = FB_SAVE_OK;
    fb->save_errno = 0;
    unsaved_guard_disarm(&fb->guard);
    if (disk == FILE_DISK_MISSING)
        fb_notice(fb, false, "%s was no longer on disk \xe2\x80\x94 written again", fb_view_name(fb));
    return true;
}

bool fb_view_changed_on_disk(FileBrowser *fb) {
    if (!fb || !fb->view_content || fb->source != FB_SOURCE_LOCAL || !fb_view_is_document(fb))
        return false;
    return file_identity_check(fb->view_path, &fb->view_disk) == FILE_DISK_CHANGED;
}

/* Put the file's text in place of the view's, keeping the mode and where the
 * reader was. Unconditional — whether the view's own edits may go is the
 * caller's question. The read is on this thread: every caller is either about
 * to write the same file from here, or was asked to reload by the loop that
 * has just stat'ed it. False, and the view untouched, when the file cannot be
 * read (gone, too large, not a file). */
static bool fb_view_reload_now(FileBrowser *fb) {
    if (!fb || !fb->view_content || fb->source != FB_SOURCE_LOCAL || !fb->view_path[0] ||
        !fb_view_is_document(fb) || fb_md_open_active(fb)) return false;
    char *content; usize len, cap; i32 lines; FileIdentity disk;
    if (!fb_file_read(fb->view_path, &content, &len, &cap, &lines, &disk)) return false;

    if (fb->view_mode == FVIEW_MARKDOWN) {
        /* The AST, and every rect and outline entry collected from it, point
         * into the text about to be freed. fb_md_reparse builds the next. */
        fb_md_parse_cancel(fb);
        if (fb->md_arena.base) arena_destroy(&fb->md_arena);
        free(fb->md_src); fb->md_src = NULL;
        fb->md_doc = NULL;
        fb->md_link_count = fb->md_glyph_count = fb->md_outline_count = 0;
        fb->md_task_count = fb->md_fold_count = 0;
        fb->md_copy_pending = false;
        fb->md_sel_active = false;
        fb->md_source_dirty = true;
    }
    editor_line_cache_drop(fb);
    free(fb->view_content);
    fb->view_content = content;
    fb->view_size = len;
    fb->view_cap = cap;
    fb->view_line_count = lines;
    fb->view_edit_gen++;
    fb->view_crlf = editor_detect_crlf(content, len);
    fb->view_disk = disk;
    fb->modified = false;
    unsaved_guard_disarm(&fb->guard);
    fb_editor_reset_undo(fb);            /* its frames are offsets into the old text */
    fb->ac_active = false;
    fb->want_valid = false;
    if (fb->view_scroll >= lines) fb->view_scroll = lines - 1;
    if (fb->view_scroll < 0) fb->view_scroll = 0;
    editor_clamp_cursor(fb);
    return true;
}

bool fb_view_reload(FileBrowser *fb) {
    if (!fb || !fb->view_content || !fb_view_is_document(fb)) return false;
    if (fb_editor_dirty(fb)) {
        /* The edits here are the only copy of themselves. They stay; the
         * save is where the two versions meet, and it asks. */
        fb_notice(fb, true, "%s changed on disk \xe2\x80\x94 not reloaded, it has unsaved changes",
                  fb_view_name(fb));
        return false;
    }
    if (!fb_view_reload_now(fb)) return false;
    if (fb->view_mode == FVIEW_MARKDOWN) fb_md_reparse(fb);
    fb_notice(fb, false, "%s changed on disk \xe2\x80\x94 reloaded", fb_view_name(fb));
    return true;
}

/* Is `path` the directory `dir`, or something inside it? */
static bool fb_path_within(const char *path, const char *dir) {
    usize n = strlen(dir);
    while (n > 1 && dir[n - 1] == '/') n--;
    return n > 0 && !strncmp(path, dir, n) && (path[n] == '\0' || path[n] == '/');
}

void fb_view_path_moved(FileBrowser *fb, const char *old_path, const char *new_path) {
    if (!fb || !old_path || !new_path || !old_path[0] || !new_path[0]) return;
    if (!fb->view_path[0] || !fb_view_is_document(fb)) return;
    char moved[FB_MAX_PATH * 2];
    if (fb_path_within(fb->view_path, old_path)) {
        /* The file itself, or a folder above it: the same tail under the
         * new name. */
        usize n = strlen(old_path);
        while (n > 1 && old_path[n - 1] == '/') n--;
        snprintf(moved, sizeof moved, "%s%s", new_path, fb->view_path + n);
    } else {
        /* Another spelling of the same file (a linked folder, /tmp against
         * /private/tmp): the inode that was loaded is the one now at
         * `new_path`. A rename keeps it. */
        struct stat st;
        if (stat(new_path, &st) != 0 || !S_ISREG(st.st_mode)) return;
        FileIdentity there = file_identity_of(&st);
        if (!file_identity_same_inode(&fb->view_disk, &there)) return;
        snprintf(moved, sizeof moved, "%s", new_path);
    }
    if (strlen(moved) >= sizeof fb->view_path) return;   /* too long to follow: detached */
    snprintf(fb->view_path, sizeof fb->view_path, "%s", moved);
    /* The language can change with the name (notes.txt -> notes.c). */
    fb->code_lang = syntax_lang_for_path(fb->view_path, strlen(fb->view_path));
    fb_syn_invalidate(fb);
}

void fb_view_path_gone(FileBrowser *fb, const char *path) {
    if (!fb || !path || !path[0] || !fb->view_content) return;
    if (!fb->view_path[0] || !fb_view_is_document(fb)) return;
    if (!fb_path_within(fb->view_path, path)) return;
    /* What is in the viewer is the only copy now: it is unsaved, whatever it
     * was a moment ago, and there is no file left for a save to disagree
     * with. */
    fb->modified = true;
    fb->view_disk = (FileIdentity){0};
    unsaved_guard_disarm(&fb->guard);
    /* The save key writes only in Edit mode: in the reader it does nothing. */
    if (fb->editor_mode)
        fb_notice(fb, false, "%s was deleted \xe2\x80\x94 " UNSAVED_SAVE_KEY " writes it again",
                  fb_view_name(fb));
    else
        fb_notice(fb, false, "%s was deleted \xe2\x80\x94 its text is still open here",
                  fb_view_name(fb));
}

/* =========================================================================
 * Editor undo/redo — journaled splices
 *
 * See the FbUndoPending note in filebrowser.h. Every mutation of view_content
 * goes through editor_apply() below, which journals the bytes it removes into
 * the pending span BEFORE moving anything; fb_editor_push_undo turns the span
 * into a frame. Undo and redo apply frames to the document with undo_apply.
 * ========================================================================= */

/* Free a single frame's owned byte buffers. */
static void undo_frame_free(struct FbUndoFrame *f) {
    if (!f) return;
    free(f->old_bytes);
    free(f->new_bytes);
    f->old_bytes = NULL;
    f->new_bytes = NULL;
    f->old_len = 0;
    f->new_len = 0;
}

static void undo_pending_free(struct FbUndoPending *p) {
    if (!p) return;
    free(p->old_bytes);
    memset(p, 0, sizeof *p);
}

/* Room for `need` more committed bytes in the pending span's old_bytes. */
static bool undo_pending_reserve(struct FbUndoPending *p, usize need) {
    usize want;
    if (!checked_add_usize(p->old_len, need, &want)) return false;
    if (want <= p->old_cap) return true;
    usize cap = p->old_cap ? p->old_cap : 64;
    while (cap < want) {
        if (cap > ((usize)-1) / 2u) { cap = want; break; }
        cap *= 2u;
    }
    char *d = realloc(p->old_bytes, cap);
    if (!d) return false;
    p->old_bytes = d;
    p->old_cap = cap;
    return true;
}

/* Journal a splice ABOUT to be made: `oldlen` bytes at `off` of the current
 * document replaced by `newlen` bytes. Reads the bytes it needs from the
 * document, which must still be untouched. The covering span widens to take
 * the edit in; the bytes between the old span and the edit are unchanged
 * since the commit, so they can be read from the document as they are. */
static bool undo_journal(FileBrowser *fb, usize off, usize oldlen, usize newlen) {
    struct FbUndoPending *p = &fb->undo_pending;
    if (!p->active) {
        memset(p, 0, sizeof *p);
        if (oldlen) {
            if (!undo_pending_reserve(p, oldlen)) return false;
            memcpy(p->old_bytes, fb->view_content + off, oldlen);
            p->old_len = oldlen;
        }
        p->start   = off;
        p->cur_len = newlen;
        p->active  = true;
        return true;
    }
    usize end  = p->start + p->cur_len;
    usize lo   = off < p->start ? off : p->start;
    usize hi   = off + oldlen > end ? off + oldlen : end;
    usize pre  = p->start - lo;           /* unchanged bytes gained before the span */
    usize post = hi - end;                /* bytes gained after it */
    usize need;
    if (!checked_add_usize(pre, post, &need) || !undo_pending_reserve(p, need)) return false;
    if (pre) {
        memmove(p->old_bytes + pre, p->old_bytes, p->old_len);
        memcpy(p->old_bytes, fb->view_content + lo, pre);
        p->old_len += pre;
    }
    if (post) {
        memcpy(p->old_bytes + p->old_len, fb->view_content + end, post);
        p->old_len += post;
    }
    p->start   = lo;
    p->cur_len = (hi - lo) - oldlen + newlen;
    return true;
}

/* Is this edit a continuation of the pending span? An edit that touches or
 * overlaps it extends the same undo unit; one somewhere else starts a new
 * unit — so Cmd+Z takes back a run of typing, not a run of typing here plus
 * a fix made twenty lines away in the same ten keystrokes. */
static bool undo_pending_adjacent(const FileBrowser *fb, usize off, usize oldlen) {
    const struct FbUndoPending *p = &fb->undo_pending;
    if (!p->active) return true;
    return off + oldlen >= p->start && off <= p->start + p->cur_len;
}

/* Apply `frame` to (*buf, *size) in place.
 * `forward` = true : state[i] → state[i+1] (replace old_bytes with new_bytes).
 * `forward` = false: state[i+1] → state[i] (replace new_bytes with old_bytes).
 * Returns true on success. On failure, (*buf, *size) are unchanged. */
static bool undo_apply(char **buf, usize *size, const struct FbUndoFrame *frame,
                        bool forward) {
    if (!buf || !*buf || !size || !frame) return false;

    usize in_len  = forward ? frame->old_len : frame->new_len;
    usize out_len = forward ? frame->new_len : frame->old_len;
    const char *out_bytes = forward ? frame->new_bytes : frame->old_bytes;

    if ((out_len > 0 && !out_bytes) || frame->offset > *size ||
        in_len > *size - frame->offset) return false;

    usize tail = *size - frame->offset - in_len;
    usize new_size, allocation_size;
    if (!checked_add_usize(*size - in_len, out_len, &new_size) ||
        !checked_add_usize(new_size, 1u, &allocation_size) ||
        new_size > (usize)INT32_MAX - 1u) return false;

    if (out_len > in_len) {
        /* Need to grow first */
        char *grown = realloc(*buf, allocation_size);
        if (!grown) return false;
        *buf = grown;
        /* Shift tail RIGHT after growing */
        memmove(*buf + frame->offset + out_len,
                *buf + frame->offset + in_len, tail);
    } else if (out_len < in_len) {
        /* Shift tail LEFT first, then shrink */
        memmove(*buf + frame->offset + out_len,
                *buf + frame->offset + in_len, tail);
        char *shrunk = realloc(*buf, allocation_size);
        if (shrunk) *buf = shrunk;  /* shrink failure is non-fatal */
    }
    /* in_len == out_len: no shift needed */

    if (out_len > 0) {
        memcpy(*buf + frame->offset, out_bytes, out_len);
    }
    (*buf)[new_size] = '\0';
    *size = new_size;
    return true;
}

/* The cursor at state[0] is recorded at the first edit of a fresh stack;
 * afterwards it moves forward with each dropped frame. */
static void fb_editor_prime_undo_base(FileBrowser *fb) {
    if (!fb || !fb->view_content) return;
    if (fb->undo_count > 0 || fb->undo_pending.active) return;
    fb->undo_base_cursor_line = fb->cursor_line;
    fb->undo_base_cursor_col  = fb->cursor_col;
}

/* Reset undo state.
 * Called on file open / close so no stale deltas survive the switch. */
void fb_editor_reset_undo(FileBrowser *fb) {
    fb->sel_on = false;
    for (i32 i = 0; i < fb->undo_count; i++) {
        undo_frame_free(&fb->undo_frames[i]);
    }
    fb->undo_count = 0;
    fb->undo_pos = 0;
    undo_pending_free(&fb->undo_pending);
}

/* Everything the undo system holds: each frame's spliced bytes plus the
 * pending span's committed bytes (at its capacity — what malloc was asked
 * for). view_content is NOT counted — it is the document, not undo state, and
 * is bounded separately by FB_EDIT_MAX_BYTES. */
usize fb_undo_retained_bytes(const FileBrowser *fb) {
    if (!fb) return 0;
    usize n = fb->undo_pending.old_cap;
    for (i32 i = 0; i < fb->undo_count; i++) {
        n += fb->undo_frames[i].old_len;
        n += fb->undo_frames[i].new_len;
    }
    return n;
}

void fb_editor_push_undo(FileBrowser *fb) {
    if (!fb->view_content) return;
    struct FbUndoPending *p = &fb->undo_pending;
    /* Nothing since the last commit: nothing to record. The journal
     * knows whether anything was edited, so a redo-then-push without an
     * edit needs no whole-document compare. */
    if (!p->active) return;
    if (p->start > fb->view_size || p->cur_len > fb->view_size - p->start) {
        undo_pending_free(p);            /* cannot happen; never build a bad frame */
        return;
    }

    /* 1) Discard any redo states above the current position. */
    while (fb->undo_count > fb->undo_pos) {
        undo_frame_free(&fb->undo_frames[fb->undo_count - 1]);
        fb->undo_count--;
    }

    /* 2) A net no-op needs no frame or budget admission. Check before
     * eviction so typing then deleting at the cap retains existing history.
     * The redo branch above is still invalidated by the attempted edit. */
    if (p->old_len == p->cur_len &&
        (p->cur_len == 0 || memcmp(p->old_bytes, fb->view_content + p->start, p->cur_len) == 0)) {
        undo_pending_free(p);
        return;
    }

    /* 3) Drop the oldest frame when the stack is full, OR when what undo
     *    retains has grown past its budget. The frame count alone is not a
     *    bound on MEMORY: a frame is a delta, which is a few bytes for
     *    typing but a whole blob for a paste. Dropping makes undo shallower
     *    rather than the editor heavier, and never touches view_content. */
    usize incoming = p->old_len + p->cur_len;
    while (fb->undo_count > 0 &&
           (fb->undo_count >= 32 ||
            fb_undo_retained_bytes(fb) + incoming > FB_UNDO_MAX_BYTES)) {
        fb->undo_base_cursor_line = fb->undo_frames[0].cursor_line;
        fb->undo_base_cursor_col  = fb->undo_frames[0].cursor_col;
        undo_frame_free(&fb->undo_frames[0]);
        memmove(&fb->undo_frames[0], &fb->undo_frames[1],
                31 * sizeof(fb->undo_frames[0]));
        memset(&fb->undo_frames[31], 0, sizeof(fb->undo_frames[31]));
        fb->undo_count--;
        if (fb->undo_pos > 0) fb->undo_pos--;
    }

    /* 4) The frame: the committed bytes kept, the current bytes copied out
     *    of the document. Copy-then-commit, so an allocation failure leaves
     *    the pending span (and the document) exactly as they were. */
    struct FbUndoFrame frame;
    memset(&frame, 0, sizeof frame);
    frame.offset  = p->start;
    frame.old_len = p->old_len;
    frame.new_len = p->cur_len;
    if (frame.new_len) {
        frame.new_bytes = malloc(frame.new_len);
        if (!frame.new_bytes) return;
        memcpy(frame.new_bytes, fb->view_content + p->start, frame.new_len);
    }
    if (frame.old_len) {
        /* Hand the pending buffer over at its length; the capacity slack is
         * not worth keeping on a frame that lives for the session. */
        char *shrunk = realloc(p->old_bytes, frame.old_len);
        frame.old_bytes = shrunk ? shrunk : p->old_bytes;
        p->old_bytes = NULL;
    }
    frame.cursor_line = fb->cursor_line;
    frame.cursor_col  = fb->cursor_col;
    undo_pending_free(p);

    fb->undo_frames[fb->undo_count] = frame;
    fb->undo_count++;
    fb->undo_pos = fb->undo_count;
}

/* Recount \n boundaries in view_content — after undo/redo, which apply a
 * frame to the whole document. Every ordinary edit counts incrementally in
 * editor_apply. */
static void fb_recount_lines(FileBrowser *fb) {
    fb->md_source_dirty = true; /* undo/redo may also relocate AST source slices */
    fb->view_edit_gen++;        /* ...and every Find match is at another offset */
    unsaved_guard_disarm(&fb->guard);
    fb->editor_line_cache_valid = false;
    fb->view_line_count = 1;
    for (usize i = 0; i < fb->view_size; i++)
        if (fb->view_content[i] == '\n') fb->view_line_count++;
    fb_syn_invalidate(fb);
}

static usize count_nl(const char *s, usize n) {
    usize c = 0;
    for (usize i = 0; i < n; i++) c += (s[i] == '\n');
    return c;
}

/* Replace oldlen bytes at off with newbytes. Record the edit for undo before modifying the
 * buffer, preserve NUL termination and line metadata, invalidate the highlighter state for the
 * affected suffix, and set modified. */
static bool editor_apply(FileBrowser *fb, usize off, usize oldlen,
                         const char *newbytes, usize newlen) {
    if (!fb || !fb->view_content || fb_md_open_active(fb)) return false;
    if (newlen > 0 && !newbytes) return false;
    if (off > fb->view_size) off = fb->view_size;
    if (oldlen > fb->view_size - off) oldlen = fb->view_size - off;
    if (oldlen == 0 && newlen == 0) return true;

    usize new_size, needed;
    if (!checked_add_usize(fb->view_size - oldlen, newlen, &new_size) ||
        !checked_add_usize(new_size, 1u, &needed)) return false;

    /* `newbytes` may point into the live editor buffer (the public splice is
     * used that way). A growth realloc would dangle it, and even without
     * growth the tail memmove below can overwrite it before memcpy reads it.
     * Stage an in-buffer source transactionally. Integer addresses avoid
     * undefined relational comparisons between unrelated allocations. */
    char *staged = NULL;
    if (newlen > 0) {
        uintptr_t base = (uintptr_t)(const void *)fb->view_content;
        uintptr_t src  = (uintptr_t)(const void *)newbytes;
        uintptr_t end  = base + fb->view_size;
        if (end >= base && src >= base && src <= end) {
            usize src_off = (usize)(src - base);
            if (src_off > fb->view_size || newlen > fb->view_size - src_off)
                return false;
            staged = malloc(newlen);
            if (!staged) return false;
            memcpy(staged, newbytes, newlen);
            newbytes = staged;
        }
    }
    if (!editor_ensure_cap(fb, needed)) { free(staged); return false; }

    /* An edit away from the pending span is a new undo unit. */
    if (!undo_pending_adjacent(fb, off, oldlen)) fb_editor_push_undo(fb);
    fb_editor_prime_undo_base(fb);
    /* Journal first, from the untouched document. A journal that cannot be
     * kept is not fatal to the EDIT: the pending span is dropped, and the
     * next commit starts from here — undo loses this run, not the document. */
    if (!undo_journal(fb, off, oldlen, newlen)) undo_pending_free(&fb->undo_pending);

    usize nl_old = count_nl(fb->view_content + off, oldlen);
    usize nl_new = count_nl(newbytes, newlen);

    /* Move the tail (including the trailing NUL) to its new position. memmove
     * handles the overlap for both grow and shrink. */
    memmove(fb->view_content + off + newlen,
            fb->view_content + off + oldlen,
            fb->view_size - off - oldlen + 1);
    if (newlen) memcpy(fb->view_content + off, newbytes, newlen);
    free(staged);

    fb->view_size = new_size;
    fb->view_content[fb->view_size] = '\0';
    fb->view_line_count += (i32)nl_new - (i32)nl_old;
    if (fb->view_line_count < 1) fb->view_line_count = 1;
    /* The line-anchor cache names an offset: it survives an edit at or after
     * it that moved no line boundary, and nothing else. */
    if (nl_new != nl_old || fb->editor_line_cache_offset > off)
        fb->editor_line_cache_valid = false;
    fb_syn_edited(fb, off);
    fb->md_source_dirty = true;
    fb->modified = true;
    /* The text is another text: what was computed from it is stale (the Find
     * match list above all — Replace splices at those offsets), and a
     * refusal that was waiting for its repeat was about the text before. */
    fb->view_edit_gen++;
    fb->sel_on = false;          /* the anchor named an offset of the old text */
    unsaved_guard_disarm(&fb->guard);
    return true;
}

/* Public splice: the shared primitive behind paste, find/replace and the
 * formatting shortcuts. Callers decide coalescing (call fb_editor_push_undo
 * around a discrete op) and reposition the cursor afterwards. */
bool fb_editor_splice(FileBrowser *fb, usize off, usize oldlen,
                      const char *newbytes, usize newlen) {
    return editor_apply(fb, off, oldlen, newbytes, newlen);
}

/* Insert UTF-8 text at the cursor as ONE coalesced edit (paste, autocomplete
 * accept, …). Every line break in it — LF, CRLF, a lone CR — becomes the
 * FILE's own, so a paste into a CRLF file keeps one kind of line ending, and
 * the cursor ends up past the inserted run. Caller owns undo coalescing
 * around this. */
void fb_editor_insert_text(FileBrowser *fb, const char *utf8, usize len) {
    if (!fb || !fb->editor_mode || !fb->view_content || !utf8 || len == 0) return;
    editor_clamp_cursor(fb);
    (void)editor_delete_selection(fb);     /* a paste replaces what is selected */

    /* Rewrite the breaks into a scratch buffer. Into a CRLF file each break
     * can grow by a byte; count them rather than doubling a large paste. */
    usize breaks = 0;
    if (fb->view_crlf)
        for (usize i = 0; i < len; i++) breaks += (utf8[i] == '\n' || utf8[i] == '\r');
    usize norm_size;
    if (len > (usize)INT32_MAX - 1u ||
        !checked_add_usize(len, breaks, &norm_size) ||
        !checked_add_usize(norm_size, 1u, &norm_size)) return;
    char *norm = malloc(norm_size);
    if (!norm) return;
    usize nlen = editor_convert_eol(utf8, len, norm, fb->view_crlf);
    norm[nlen] = '\0';
    if (nlen == 0) { free(norm); return; }

    char *line_start = editor_line_start(fb, fb->cursor_line);
    usize off = (usize)(line_start - fb->view_content) + (usize)fb->cursor_col;

    if (!fb_editor_splice(fb, off, 0, norm, nlen)) { free(norm); return; }

    /* Advance the cursor: count inserted newlines to land at the tail. */
    i32 added_lines = 0; usize last_nl = (usize)-1;
    for (usize i = 0; i < nlen; i++)
        if (norm[i] == '\n') { added_lines++; last_nl = i; }
    if (added_lines == 0) {
        fb->cursor_col += (i32)nlen;
    } else {
        fb->cursor_line += added_lines;
        fb->cursor_col = (i32)(nlen - last_nl - 1);
    }
    free(norm);
    editor_hfollow(fb);
}

/* Byte offset of the cursor within view_content (line start + cursor_col). */
usize fb_editor_cursor_offset(const FileBrowser *fb) {
    if (!fb || !fb->view_content) return 0;
    usize at = 0;
    for (i32 i = 0; i < fb->cursor_line && at < fb->view_size; i++) {
        usize brk = editor_line_break(fb->view_content, fb->view_size, at);
        at = brk < fb->view_size ? brk + 1 : brk;
    }
    usize off = at + (usize)(fb->cursor_col > 0 ? fb->cursor_col : 0);
    if (off > fb->view_size) off = fb->view_size;
    return off;
}

/* Document statistics for the editor status readout: codepoint count, word
 * count (whitespace-delimited runs), and an estimated read time at 200 wpm. */
static void fb_editor_doc_stats(const FileBrowser *fb, u32 *words, u32 *chars,
                                u32 *read_min) {
    u32 w = 0, c = 0; bool in_word = false;
    if (fb && fb->view_content) {
        const u8 *p = (const u8 *)fb->view_content;
        usize n = fb->view_size;
        for (usize i = 0; i < n; i++) {
            u8 b = p[i];
            if ((b & 0xC0) != 0x80) c++;   /* UTF-8 lead bytes == codepoints */
            bool ws = (b==' '||b=='\t'||b=='\n'||b=='\r'||b=='\f'||b=='\v');
            if (!ws && !in_word) { w++; in_word = true; }
            else if (ws) in_word = false;
        }
    }
    if (words) *words = w;
    if (chars) *chars = c;
    if (read_min) *read_min = (w + 199) / 200;   /* ceil at 200 wpm; 0 → 0 */
}

/* Paste the system clipboard at the cursor as a single undo unit. */
void fb_editor_paste(FileBrowser *fb) {
    if (!fb || !fb->editor_mode || !fb->view_content) return;
    const char *clip = platform_clipboard_get();   /* internal buffer — do not free */
    if (!clip || !clip[0]) return;
    fb_editor_push_undo(fb);                 /* flush pending typed edits to their own frame */
    fb_editor_insert_text(fb, clip, strlen(clip));
    fb_editor_push_undo(fb);                 /* commit the paste as its own undo unit */
}

/* Wrap a span with `open`/`close` markers — Cmd+B, Cmd+I, Cmd+Shift+C. Over a
 * selection it wraps the selection (and keeps it, so the next key can wrap it
 * again), or takes the markers off when the selection already has them, round
 * it or inside it. With none, insert the pair at the cursor and park the caret
 * between them, so what is typed next is inside the emphasis. open/close are
 * ASCII markdown markers (`**`, `*`, `` ` ``). One undo unit. */
void fb_editor_wrap_or_insert(FileBrowser *fb, const char *open, const char *close) {
    if (!fb || !fb->editor_mode || !fb->view_content || !open || !close) return;
    editor_clamp_cursor(fb);
    usize olen = strlen(open), clen = strlen(close);
    if (olen + clen >= 16) return;

    if (ed_sel_valid(fb)) {
        i32 l0, c0, l1, c1;
        ed_sel_order(fb, &l0, &c0, &l1, &c1);
        bool caret_last = fb->cursor_line > fb->sel_line ||
                          (fb->cursor_line == fb->sel_line && fb->cursor_col >= fb->sel_col);
        usize a = ed_off(fb, l0, c0), b = ed_off(fb, l1, c1);
        const char *t = fb->view_content;
        i32 last_len = editor_line_len(fb, l1);
        /* A single mark is not taken off a doubled one: "*" round "**x**" is the
         * bold's, not an emphasis to switch off. */
        bool out = b > a && (i32)olen <= c0 && c1 + (i32)clen <= last_len &&
                   memcmp(t + a - olen, open, olen) == 0 && memcmp(t + b, close, clen) == 0 &&
                   !(olen == 1 && a >= 2 && t[a - 2] == open[0]) &&
                   !(clen == 1 && b + 1 < fb->view_size && t[b + 1] == close[0]);
        bool in = !out && b - a >= olen + clen &&
                  memcmp(t + a, open, olen) == 0 && memcmp(t + b - clen, close, clen) == 0;
        /* ONE splice over the span and the marks that are changed beside it. */
        usize lo = out ? a - olen : a, hi = out ? b + clen : b;
        char *buf = malloc((b - a) + olen + clen + 1u);
        if (!buf) return;
        usize w = 0;
        i32 same = l0 == l1 ? 1 : 0;
        i32 n0 = c0, n1 = c1;
        if (out) {
            memcpy(buf, t + a, b - a); w = b - a;
            n0 = c0 - (i32)olen;
            n1 = c1 - (same ? (i32)olen : 0);
        } else if (in) {
            memcpy(buf, t + a + olen, b - a - olen - clen); w = b - a - olen - clen;
            n0 = c0;
            n1 = c1 - (i32)clen - (same ? (i32)olen : 0);
        } else {
            memcpy(buf, open, olen); w = olen;
            memcpy(buf + w, t + a, b - a); w += b - a;
            memcpy(buf + w, close, clen); w += clen;
            n0 = c0 + (i32)olen;
            n1 = c1 + (same ? (i32)olen : 0);
        }
        fb_editor_push_undo(fb);
        bool ok = editor_apply(fb, lo, hi - lo, buf, w);
        free(buf);
        if (!ok) return;
        fb->cursor_line = caret_last ? l1 : l0;
        fb->cursor_col  = caret_last ? n1 : n0;
        ed_sel_set(fb, caret_last ? l0 : l1, caret_last ? n0 : n1);
        editor_clamp_cursor(fb);
        fb_editor_push_undo(fb);
        return;
    }

    fb_editor_push_undo(fb);
    char buf[16];
    usize markers_len;
    if (!checked_add_usize(olen, clen, &markers_len) || markers_len >= sizeof buf) return;
    memcpy(buf, open, olen);
    memcpy(buf + olen, close, clen);
    char *ls = editor_line_start(fb, fb->cursor_line);
    usize off = (usize)(ls - fb->view_content) + (usize)fb->cursor_col;
    if (!fb_editor_splice(fb, off, 0, buf, markers_len)) return;
    fb->cursor_col += (i32)olen;             /* caret between the markers */
    fb_editor_push_undo(fb);
}

/* Cmd+K: a markdown link. Over a selection the text becomes the link's text
 * and the caret waits in the address; over a web address it becomes the
 * address, and the caret waits in the text. With none, the skeleton "[]()" at
 * the cursor with the caret inside the brackets. */
void fb_editor_make_link(FileBrowser *fb) {
    if (!fb || !fb->editor_mode || !fb->view_content) return;
    editor_clamp_cursor(fb);
    if (ed_sel_valid(fb) && fb->sel_line == fb->cursor_line) {
        usize a, b;
        if (fb_editor_sel_range(fb, &a, &b)) {
            usize n = b - a;
            char *sel = malloc(n + 1u);
            char *buf = malloc(n + 8u);
            if (sel && buf) {
                memcpy(sel, fb->view_content + a, n);
                sel[n] = '\0';
                bool url = (n > 7 && (strncmp(sel, "http://", 7) == 0 || strncmp(sel, "https://", 8) == 0)) &&
                           strpbrk(sel, " \t") == NULL;
                usize m = 0;
                i32 caret_col;
                i32 start = fb->sel_col < fb->cursor_col ? fb->sel_col : fb->cursor_col;
                if (url) {
                    memcpy(buf, "[](", 3); m = 3;
                    memcpy(buf + m, sel, n); m += n;
                    buf[m++] = ')';
                    caret_col = start + 1;                  /* in the brackets */
                } else {
                    buf[m++] = '[';
                    memcpy(buf + m, sel, n); m += n;
                    memcpy(buf + m, "]()", 3); m += 3;
                    caret_col = start + (i32)n + 3;         /* in the parentheses */
                }
                fb_editor_push_undo(fb);
                if (editor_apply(fb, a, n, buf, m)) {
                    fb->cursor_col = caret_col;
                    editor_clamp_cursor(fb);
                    fb_editor_push_undo(fb);
                }
            }
            free(sel);
            free(buf);
            return;
        }
    }
    fb_editor_push_undo(fb);
    char *ls = editor_line_start(fb, fb->cursor_line);
    usize off = (usize)(ls - fb->view_content) + (usize)fb->cursor_col;
    if (!fb_editor_splice(fb, off, 0, "[]()", 4)) return;
    fb->cursor_col += 1;                     /* between the [ ] */
    fb_editor_push_undo(fb);
}

/* Toggle a line-leading marker ("> ", "- ", "1. ") on the cursor's line. */
void fb_editor_toggle_line_prefix(FileBrowser *fb, const char *prefix) {
    if (!fb || !fb->editor_mode || !fb->view_content || !prefix) return;
    editor_clamp_cursor(fb);
    fb_editor_push_undo(fb);
    char *ls = editor_line_start(fb, fb->cursor_line);
    usize line_off = (usize)(ls - fb->view_content);
    usize plen = strlen(prefix);
    i32 ll = editor_line_len(fb, fb->cursor_line);
    if ((usize)ll >= plen && memcmp(ls, prefix, plen) == 0) {
        /* Remove it. */
        fb_editor_splice(fb, line_off, plen, NULL, 0);
        fb->cursor_col = fb->cursor_col > (i32)plen ? fb->cursor_col - (i32)plen : 0;
    } else {
        fb_editor_splice(fb, line_off, 0, prefix, plen);
        fb->cursor_col += (i32)plen;
    }
    fb_editor_push_undo(fb);
}

void fb_editor_undo(FileBrowser *fb) {
    if (!fb || !fb->view_content) return;

    /* Capture any draft edits the user made past the top of the stack so
     * they become redoable. This must run BEFORE the empty-stack check:
     * fresh typing only pushes a frame every ~10 edits (editor_pre_edit),
     * so with undo_pos == 0 the pending keystrokes live solely in
     * view_content and an early return would make Cmd+Z a silent no-op
     * right after typing. */
    fb_editor_push_undo(fb);          /* the pending run becomes the frame Cmd+Z takes back */
    if (fb->undo_pos <= 0) return;

    const struct FbUndoFrame *frame = &fb->undo_frames[fb->undo_pos - 1];
    if (!undo_apply(&fb->view_content, &fb->view_size, frame, false)) return;
    fb->view_cap = fb->view_size + 1;  /* undo_apply reallocs to exactly this */
    fb->undo_pos--;

    /* Cursor at state[undo_pos] — stored in the frame just passed over, or
     * in the base for state[0]. */
    if (fb->undo_pos == 0) {
        fb->cursor_line = fb->undo_base_cursor_line;
        fb->cursor_col  = fb->undo_base_cursor_col;
    } else {
        fb->cursor_line = fb->undo_frames[fb->undo_pos - 1].cursor_line;
        fb->cursor_col  = fb->undo_frames[fb->undo_pos - 1].cursor_col;
    }

    fb_recount_lines(fb);
    fb->sel_on = false;
    fb->modified = true;
    editor_scroll_to_cursor(fb);   /* the restored cursor may be off-screen */
}

void fb_editor_redo(FileBrowser *fb) {
    if (!fb || !fb->view_content) return;
    /* Edits since the last undo make the redo branch unreachable: committing
     * them truncates it, which is what every editor does. */
    if (fb->undo_pending.active) fb_editor_push_undo(fb);
    if (fb->undo_pos >= fb->undo_count) return;
    const struct FbUndoFrame *frame = &fb->undo_frames[fb->undo_pos];
    if (!undo_apply(&fb->view_content, &fb->view_size, frame, true)) return;
    fb->view_cap = fb->view_size + 1;  /* undo_apply reallocs to exactly this */
    fb->undo_pos++;
    fb->cursor_line = frame->cursor_line;
    fb->cursor_col  = frame->cursor_col;
    fb_recount_lines(fb);
    fb->sel_on = false;
    fb->modified = true;
    editor_scroll_to_cursor(fb);   /* the restored cursor may be off-screen */
}

char *fb_editor_get_line(FileBrowser *fb, i32 line) {
    return editor_line_start(fb, line);
}

void fb_editor_duplicate_line(FileBrowser *fb) {
    if (!fb->view_content || !fb->editor_mode) return;
    fb_editor_push_undo(fb);

    char *line = editor_line_start(fb, fb->cursor_line);
    i32 len = editor_line_len(fb, fb->cursor_line);
    /* Find end of line (after \n) */
    char *line_end = line + len + editor_line_eol(fb, fb->cursor_line);   /* with its break */
    i32 insert_len = (i32)(line_end - line);
    if (insert_len < 0) return;
    usize line_off   = (usize)(line - fb->view_content);
    usize insert_pos = (usize)(line_end - fb->view_content);
    if (editor_line_eol(fb, fb->cursor_line) == 0) {
        /* The last line, with no break after it: the copy goes on a line of
         * its own, after a break in the file's own ending, rather than
         * glued to the line ("foofoo"). */
        usize n = (usize)len;
        const char *brk = fb->view_crlf ? "\r\n" : "\n";
        usize bn = strlen(brk);
        char *copy = malloc(bn + n);
        if (!copy) return;
        memcpy(copy, brk, bn);
        memcpy(copy + bn, fb->view_content + line_off, n);
        bool ok = editor_apply(fb, insert_pos, 0, copy, bn + n);
        free(copy);
        if (!ok) return;
        fb->cursor_line++;
        fb_editor_push_undo(fb);
        return;
    }
    /* The source is inside the document: editor_apply stages it. */
    if (!editor_apply(fb, insert_pos, 0, fb->view_content + line_off, (usize)insert_len)) return;
    fb->cursor_line++;
    fb_editor_push_undo(fb);
}

void fb_editor_cut_line(FileBrowser *fb) {
    if (!fb->view_content || !fb->editor_mode) return;
    fb_editor_push_undo(fb);

    char *line = editor_line_start(fb, fb->cursor_line);
    i32 len = editor_line_len(fb, fb->cursor_line);
    char *line_end = line + len + editor_line_eol(fb, fb->cursor_line);   /* with its break */
    i32 cut_len = (i32)(line_end - line);

    /* Copy the line to the clipboard, and remove it only if that worked: a
     * line of any length reaches the clipboard before it is deleted, and a
     * refused write deletes nothing. */
    if (cut_len < 0) return;
    char *tmp = (char *)malloc((usize)cut_len + 1u);
    if (!tmp) return;
    memcpy(tmp, line, (usize)cut_len);
    tmp[cut_len] = '\0';
    bool copied = platform_clipboard_set(tmp) == PLATFORM_CLIP_OK;
    free(tmp);
    if (!copied) return;

    /* Remove line */
    usize cut_pos = (usize)(line - fb->view_content);
    if (!editor_apply(fb, cut_pos, (usize)cut_len, NULL, 0)) return;
    if (fb->cursor_line >= fb->view_line_count && fb->view_line_count > 0)
        fb->cursor_line = fb->view_line_count - 1;
    fb->cursor_col = 0;
    fb_editor_push_undo(fb);
}

void fb_editor_copy_line(FileBrowser *fb) {
    if (!fb->view_content || !fb->editor_mode) return;

    char *line = editor_line_start(fb, fb->cursor_line);
    i32 len = editor_line_len(fb, fb->cursor_line);
    char *line_end = line + len + editor_line_eol(fb, fb->cursor_line);   /* with its break */
    i32 copy_len = (i32)(line_end - line);

    if (copy_len < 0) return;
    char *tmp = (char *)malloc((usize)copy_len + 1u);
    if (!tmp) return;
    memcpy(tmp, line, (usize)copy_len);
    tmp[copy_len] = '\0';
    (void)platform_clipboard_set(tmp);
    free(tmp);
}

void fb_editor_goto_line_start(FileBrowser *fb) {
    editor_clamp_cursor(fb);
    fb->cursor_col = 0;
    editor_hfollow(fb);
}

void fb_editor_goto_line_end(FileBrowser *fb) {
    editor_clamp_cursor(fb);
    fb->cursor_col = editor_line_len(fb, fb->cursor_line);
    editor_hfollow(fb);
}

/* ---- macOS-native word / line / document editing -----------------------
 * cursor_col is a byte offset within the line. A "word" is a run of
 * non-whitespace, matching readline / Option-Backspace expectations; UTF-8
 * continuation/lead bytes (>= 0x80) count as word chars so a glyph is never
 * split. Mirrors fb_editor_move's view_scroll follow so the caret stays
 * on-screen after a long jump. */
static inline bool ed_is_ws(char c) { return c == ' ' || c == '\t'; }

static void editor_scroll_to_cursor(FileBrowser *fb) {
    i32 visible = fb->code_visible_lines > 0 ? fb->code_visible_lines : 30;
    if (fb->cursor_line < fb->view_scroll) fb->view_scroll = fb->cursor_line;
    if (fb->cursor_line >= fb->view_scroll + visible)
        fb->view_scroll = fb->cursor_line - visible + 1;
    if (fb->view_scroll < 0) fb->view_scroll = 0;
    editor_hfollow(fb);
}

void fb_editor_move_word_left(FileBrowser *fb) {
    if (!fb->editor_mode || !fb->view_content) return;
    editor_clamp_cursor(fb);
    if (fb->cursor_col == 0) {
        if (fb->cursor_line > 0) {
            fb->cursor_line--;
            fb->cursor_col = editor_line_len(fb, fb->cursor_line);
        }
    } else {
        char *ls = editor_line_start(fb, fb->cursor_line);
        i32 c = fb->cursor_col;
        while (c > 0 &&  ed_is_ws(ls[c-1])) c--;
        while (c > 0 && !ed_is_ws(ls[c-1])) c--;
        fb->cursor_col = c;
    }
    editor_scroll_to_cursor(fb);
}

void fb_editor_move_word_right(FileBrowser *fb) {
    if (!fb->editor_mode || !fb->view_content) return;
    editor_clamp_cursor(fb);
    i32 len = editor_line_len(fb, fb->cursor_line);
    if (fb->cursor_col >= len) {
        if (fb->cursor_line < fb->view_line_count - 1) {
            fb->cursor_line++;
            fb->cursor_col = 0;
        }
    } else {
        char *ls = editor_line_start(fb, fb->cursor_line);
        i32 c = fb->cursor_col;
        while (c < len &&  ed_is_ws(ls[c])) c++;
        while (c < len && !ed_is_ws(ls[c])) c++;
        fb->cursor_col = c;
    }
    editor_scroll_to_cursor(fb);
}

/* Delete a byte run [a,b) within the current line and place the cursor at a. */
static void editor_delete_run(FileBrowser *fb, char *line_start, i32 a, i32 b) {
    if (b <= a) return;
    editor_pre_edit(fb);
    usize off = (usize)(line_start - fb->view_content) + (usize)a;
    if (!editor_apply(fb, off, (usize)(b - a), NULL, 0)) return;
    fb->cursor_col = a;
}

void fb_editor_delete_word_left(FileBrowser *fb) {
    if (!fb->editor_mode || !fb->view_content) return;
    editor_clamp_cursor(fb);
    if (editor_delete_selection(fb)) return;
    if (fb->cursor_col == 0) { fb_editor_backspace(fb); return; }  /* joins lines */
    char *ls = editor_line_start(fb, fb->cursor_line);
    i32 c = fb->cursor_col;
    while (c > 0 &&  ed_is_ws(ls[c-1])) c--;
    while (c > 0 && !ed_is_ws(ls[c-1])) c--;
    editor_delete_run(fb, ls, c, fb->cursor_col);
}

void fb_editor_delete_word_right(FileBrowser *fb) {
    if (!fb->editor_mode || !fb->view_content) return;
    editor_clamp_cursor(fb);
    if (editor_delete_selection(fb)) return;
    i32 len = editor_line_len(fb, fb->cursor_line);
    if (fb->cursor_col >= len) { fb_editor_delete(fb); return; }   /* joins lines */
    char *ls = editor_line_start(fb, fb->cursor_line);
    i32 c = fb->cursor_col;
    while (c < len &&  ed_is_ws(ls[c])) c++;
    while (c < len && !ed_is_ws(ls[c])) c++;
    i32 from = fb->cursor_col;
    editor_delete_run(fb, ls, from, c);
    fb->cursor_col = from;   /* forward delete leaves the caret in place */
}

void fb_editor_delete_to_line_start(FileBrowser *fb) {
    if (!fb->editor_mode || !fb->view_content) return;
    editor_clamp_cursor(fb);
    if (editor_delete_selection(fb)) return;
    if (fb->cursor_col == 0) { fb_editor_backspace(fb); return; }
    char *ls = editor_line_start(fb, fb->cursor_line);
    editor_delete_run(fb, ls, 0, fb->cursor_col);
}

void fb_editor_goto_doc_start(FileBrowser *fb) {
    if (!fb->editor_mode || !fb->view_content) return;
    fb->cursor_line = 0;
    fb->cursor_col = 0;
    fb->view_scroll = 0;
    fb->view_scroll_x = 0;
    if (fb_md_edit_active(fb)) { fb->view_scroll_px = 0; fb->live_follow = true; }
}

void fb_editor_goto_doc_end(FileBrowser *fb) {
    if (!fb->editor_mode || !fb->view_content) return;
    fb->cursor_line = fb->view_line_count > 0 ? fb->view_line_count - 1 : 0;
    fb->cursor_col = editor_line_len(fb, fb->cursor_line);
    editor_scroll_to_cursor(fb);
}

/* Push undo before destructive edits */
static void editor_pre_edit(FileBrowser *fb) {
    /* Commit the pending run every ~10 keystrokes, so a long run of typing is
     * several undo units rather than one. The counter lives on the instance, so
     * edits in one pane do not skew another pane's timing. The journal itself
     * is kept by editor_apply, which every edit goes through. */
    if (++fb->edit_counter >= 10) {
        fb_editor_push_undo(fb);
        fb->edit_counter = 0;
    }
}

/* =========================================================================
 * Input handling
 * ========================================================================= */

/* Move the code/plain-text editor caret to the clicked framebuffer point.
 * Maps through the geometry the last fb_render_viewer cached (code_text_*),
 * so the click lands on exactly the glyph grid that was drawn — including
 * the current vertical AND horizontal scroll. The clicked visual column is
 * converted to a byte offset (cursor_col) with a UTF-8 walk. */
bool fb_editor_click_caret(FileBrowser *fb, f32 mx, f32 my) {
    if (!fb || !fb->editor_mode || !fb->view_content) return false;
    if (fb->view_mode != FVIEW_TEXT && fb->view_mode != FVIEW_CODE) return false;
    if (fb->code_cw <= 0.0f || fb->code_ch <= 0.0f) return false;  /* not rendered yet */

    i32 row = (i32)((my - fb->code_text_y) / fb->code_ch);
    if (row < 0) row = 0;
    i32 line = fb->view_scroll + row;
    if (line >= fb->view_line_count) line = fb->view_line_count - 1;
    if (line < 0) line = 0;

    /* The point as a fractional column of the line, and from there to the
     * nearer edge of the cell under it — so clicking the right half of a
     * glyph puts the caret after it, as native text fields do. The cells are
     * the painter's (ui/editor_text.h): a tab is as wide as it was drawn, and
     * a byte that is not UTF-8 is the cell it was drawn in. */
    f32 want = (mx - fb->code_text_x) / fb->code_cw + (f32)fb->view_scroll_x;

    const char *ls = editor_line_start(fb, line);
    if (!ls) return false;
    i32 ll = editor_line_len(fb, line);
    fb->cursor_line = line;
    fb->cursor_col  = editor_byte_at_point(ls, ll, want, FB_TAB_COLS);
    return true;
}

bool fb_handle_click(FileBrowser *fb, f32 rel_x, f32 rel_y, f32 w, f32 h, f32 dpi) {
    (void)w; (void)rel_x; (void)h;
    if (rel_y < fb_list_hdr_h(dpi)) return true; /* header click */

    i32 idx = fb_list_entry_at(fb, rel_y, dpi);
    if (idx >= 0 && idx < fb->entry_count) {
        fb->selected = idx;
        fb_sel_clear(fb);
        fb_sel_set(fb, idx, true);
        fb->selection_anchor = idx;
        fb_open_file(fb, idx);
        return true;
    }
    return false;
}

/* =========================================================================
 * Rendered-markdown text selection
 *
 * md_render fills fb->md_glyph_rects each frame in reading order (document
 * order, framebuffer px). Selection endpoints are stored in CONTENT space
 * (screen − origin + scroll-at-mouse-down) so they're stable across scroll
 * and reflow. Each resolve maps the captured glyph rects back into doc
 * space using `md_glyph_scroll_px` — the scroll the rects were filled at,
 * not whatever scroll is current — so endpoints stay aligned with the
 * actual glyphs even when the user is scrolling and selecting at once.
 * ========================================================================= */

/* Map a content-space point to a reading-order insertion index (0..count).
 *
 * Uses `md_glyph_scroll_px` — the scroll value in effect when md_render
 * filled the rect buffer — not the current `view_scroll_px`. The rects'
 * framebuffer y reflects that older scroll; subtracting origin and adding
 * THAT scroll cancels back to doc-space coordinates. Using the current
 * scroll instead would offset every doc-space y by (now − then), which
 * mid-scroll-drag selects the wrong glyphs. */
static u32 md_sel_index_in(const FileBrowser *fb, const MdGlyphRect *rects,
                           u32 count, f32 sc, f32 px, f32 py) {
    f32 ox = fb->md_origin_x, oy = fb->md_origin_y;
    u32 idx = 0;
    for (u32 i = 0; i < count; i++) {
        const MdGlyphRect *g = &rects[i];
        f32 gx = g->x - ox;          /* content x */
        f32 gy = g->y - oy + sc;     /* content y */
        bool before;
        if (gy + g->h <= py)      before = true;                 /* line above */
        else if (gy <= py)        before = (px >= gx + g->w * 0.5f); /* same line */
        else                      before = false;                /* line below */
        if (before) idx = i + 1;
    }
    return idx;
}

static u32 md_sel_index_at(const FileBrowser *fb, f32 px, f32 py) {
    return md_sel_index_in(fb, fb->md_glyph_rects, fb->md_glyph_count,
                           fb->md_glyph_scroll_px, px, py);
}

/* The selection as text, over whatever glyph list it is handed: the
 * viewport-only one for the highlight's sake, the whole-document one the
 * copy frame collects (see fb_md_copy_finish). */
static char *md_sel_text_from(const FileBrowser *fb, const MdGlyphRect *rects,
                              u32 count, f32 sc) {
    if (!fb->md_sel_active || !rects || count == 0) return NULL;
    u32 a = md_sel_index_in(fb, rects, count, sc, fb->md_sel_ax, fb->md_sel_ay);
    u32 b = md_sel_index_in(fb, rects, count, sc, fb->md_sel_hx, fb->md_sel_hy);
    if (a == b) return NULL;
    u32 lo = a < b ? a : b, hi = a < b ? b : a;
    /* The text is md_render's to make: it knows which row changes are the
     * document's line breaks and which are only this window's wrapping, where
     * a tab or a blank line stood, and what is chrome (md_glyphs_text). A
     * break at every change of glyph y — what this did — copied a wrapped
     * code line as several lines with an arrow at the end of each. The first
     * call sizes the buffer. */
    usize len = md_glyphs_text(rects, lo, hi, NULL, 0);
    usize cap;
    if (!checked_add_usize(len, 1u, &cap)) return NULL;
    char *buf = (char *)malloc(cap);
    if (!buf) return NULL;
    md_glyphs_text(rects, lo, hi, buf, cap);
    return buf;
}

/* Resolve the stored selection to a [lo, hi) glyph index range. */
static bool md_sel_range(const FileBrowser *fb, u32 *lo, u32 *hi) {
    if (!fb->md_sel_active || !fb->md_glyph_rects || fb->md_glyph_count == 0)
        return false;
    u32 a = md_sel_index_at(fb, fb->md_sel_ax, fb->md_sel_ay);
    u32 b = md_sel_index_at(fb, fb->md_sel_hx, fb->md_sel_hy);
    if (a == b) return false;
    *lo = a < b ? a : b;
    *hi = a < b ? b : a;
    return true;
}

static void fb_md_draw_selection(FileBrowser *fb, Renderer *r, const Theme *theme) {
    u32 lo, hi;
    if (!md_sel_range(fb, &lo, &hi)) return;
    Color sel = theme ? theme->selection : (Color){0.20f, 0.30f, 0.50f, 1.0f};
    /* Drawn before md_render's text (see fb_render_viewer), so the highlight
     * sits behind every glyph — body and headings alike. Kept translucent so
     * block backgrounds md_render paints on top (code fences, callouts) don't
     * fully bury a selection that runs through them. */
    sel.a = 0.40f;
    /* Coalesce selected glyphs into one rect per visual line. Glyphs on a
     * line share a y from md_render's line layout, but inline elements with
     * a different cell height (mixed font sizes, sub/superscript, future
     * inline images) can shift the baseline by a fraction of a pixel — so
     * compare with a half-pixel tolerance rather than `==`. A real line
     * break is at least ~ch apart, comfortably above the tolerance. */
    u32 i = lo;
    while (i < hi) {
        f32 line_y = fb->md_glyph_rects[i].y;
        f32 x0 = fb->md_glyph_rects[i].x;
        f32 x1 = x0 + fb->md_glyph_rects[i].w;
        f32 hgt = fb->md_glyph_rects[i].h;
        u32 j = i + 1;
        while (j < hi && fabsf(fb->md_glyph_rects[j].y - line_y) < 0.5f) {
            const MdGlyphRect *g = &fb->md_glyph_rects[j];
            if (g->x < x0)          x0 = g->x;
            if (g->x + g->w > x1)   x1 = g->x + g->w;
            if (g->h > hgt)         hgt = g->h;
            j++;
        }
        renderer_draw_rect(r, x0, line_y, x1 - x0, hgt, sel);
        i = j;
    }
}

void fb_md_selection_begin(FileBrowser *fb, f32 mx, f32 my) {
    if (!fb || fb->view_mode != FVIEW_MARKDOWN || fb->md_raw_mode) return;
    f32 cx = mx - fb->md_origin_x;
    f32 cy = my - fb->md_origin_y + fb->view_scroll_px;
    fb->md_sel_ax = fb->md_sel_hx = cx;
    fb->md_sel_ay = fb->md_sel_hy = cy;
    fb->md_sel_active = true;
}

void fb_md_selection_update(FileBrowser *fb, f32 mx, f32 my) {
    if (!fb || !fb->md_sel_active) return;
    fb->md_sel_hx = mx - fb->md_origin_x;
    fb->md_sel_hy = my - fb->md_origin_y + fb->view_scroll_px;
}

void fb_md_selection_clear(FileBrowser *fb) {
    if (fb) fb->md_sel_active = false;
}

/* ---- Selecting a unit of the rendered note ----------------------------------
 * The selection is two POINTS in content space, resolved to glyph indices at
 * every use (md_sel_index_in). A unit is selected by putting the anchor just
 * inside the first glyph's left half and the head just inside the last glyph's
 * right half: the indices that come out are [first, last + 1). */

static bool md_same_row(const MdGlyphRect *a, const MdGlyphRect *b) {
    return fabsf(a->y - b->y) < 0.5f;
}

/* The glyph under a point (content space), else the nearest on its row. */
static bool md_glyph_near(const FileBrowser *fb, f32 px, f32 py, u32 *out) {
    const MdGlyphRect *g = fb->md_glyph_rects;
    if (!g || fb->md_glyph_count == 0) return false;
    f32 ox = fb->md_origin_x, oy = fb->md_origin_y, sc = fb->md_glyph_scroll_px;
    bool found = false;
    f32 best = 1e30f;
    for (u32 i = 0; i < fb->md_glyph_count; i++) {
        if (g[i].cp == 0) continue;                         /* a place, not a glyph */
        f32 gx = g[i].x - ox, gy = g[i].y - oy + sc;
        if (py < gy || py >= gy + g[i].h) continue;
        f32 d = px < gx ? gx - px : px > gx + g[i].w ? px - (gx + g[i].w) : 0.0f;
        if (d < best) { best = d; *out = i; found = true; }
    }
    return found;
}

/* 0 blank, 1 a letter, digit or "_" (anything not ASCII, as in the editor), 2 a mark. */
static i32 md_glyph_class(u32 cp) {
    if (cp == ' ' || cp == '\t' || cp == 0 || cp == 0xA0) return 0;
    if (cp >= 0x80 || (cp < 0x80 && (isalnum((int)cp) || cp == '_'))) return 1;
    return 2;
}

static void md_select_glyphs(FileBrowser *fb, u32 a, u32 b) {
    const MdGlyphRect *g = fb->md_glyph_rects;
    f32 ox = fb->md_origin_x, oy = fb->md_origin_y, sc = fb->md_glyph_scroll_px;
    fb->md_sel_ax = g[a].x - ox + g[a].w * 0.25f;
    fb->md_sel_ay = g[a].y - oy + sc + g[a].h * 0.5f;
    fb->md_sel_hx = g[b].x - ox + g[b].w * 0.75f;
    fb->md_sel_hy = g[b].y - oy + sc + g[b].h * 0.5f;
    fb->md_sel_active = true;
}

bool fb_md_select_word_at(FileBrowser *fb, f32 mx, f32 my) {
    if (!fb || fb->view_mode != FVIEW_MARKDOWN || fb->md_raw_mode) return false;
    u32 i;
    if (!md_glyph_near(fb, mx - fb->md_origin_x, my - fb->md_origin_y + fb->view_scroll_px, &i)) return false;
    const MdGlyphRect *g = fb->md_glyph_rects;
    i32 cl = md_glyph_class(g[i].cp);
    u32 a = i, b = i;
    while (a > 0 && md_same_row(&g[a - 1], &g[i]) && md_glyph_class(g[a - 1].cp) == cl) a--;
    while (b + 1 < fb->md_glyph_count && md_same_row(&g[b + 1], &g[i]) && md_glyph_class(g[b + 1].cp) == cl) b++;
    md_select_glyphs(fb, a, b);
    return true;
}

bool fb_md_select_line_at(FileBrowser *fb, f32 mx, f32 my) {
    if (!fb || fb->view_mode != FVIEW_MARKDOWN || fb->md_raw_mode) return false;
    u32 i;
    if (!md_glyph_near(fb, mx - fb->md_origin_x, my - fb->md_origin_y + fb->view_scroll_px, &i)) return false;
    const MdGlyphRect *g = fb->md_glyph_rects;
    u32 n = fb->md_glyph_count;
    /* The row, then every row above that only continues the line above it, and
     * every row below that only continues this one: a wrapped paragraph is one line. */
    u32 a = i;
    for (;;) {
        while (a > 0 && md_same_row(&g[a - 1], &g[a])) a--;
        if (a == 0 || !(g[a].flags & MD_GLYPH_WRAP)) break;
        a--;
    }
    u32 b = i;
    for (;;) {
        while (b + 1 < n && md_same_row(&g[b + 1], &g[b])) b++;
        if (b + 1 >= n || !(g[b + 1].flags & MD_GLYPH_WRAP)) break;
        b++;
    }
    md_select_glyphs(fb, a, b);
    return true;
}

char *fb_md_selection_text_visible(FileBrowser *fb) {
    if (!fb || !fb_md_selection_active(fb)) return NULL;
    return md_sel_text_from(fb, fb->md_glyph_rects, fb->md_glyph_count, fb->md_glyph_scroll_px);
}

void fb_md_select_all(FileBrowser *fb) {
    if (!fb || fb->view_mode != FVIEW_MARKDOWN || fb->md_raw_mode) return;
    /* Before the first glyph, after the last: whatever the glyph list is. */
    fb->md_sel_ax = fb->md_sel_ay = -1.0f;
    fb->md_sel_hx = fb->md_sel_hy = 1.0e7f;
    fb->md_sel_active = true;
}

bool fb_md_selection_active(const FileBrowser *fb) {
    if (!fb || !fb->md_sel_active) return false;
    /* Defensive gate matching fb_md_selection_begin: the selection is only
     * meaningful while the rendered Markdown viewer is on screen. If a stale
     * md_sel_active flag survives a viewer close or a non-Markdown open,
     * never report it as active. */
    if (fb->view_mode != FVIEW_MARKDOWN || fb->md_raw_mode) return false;
    /* The two POINTS, not the visible glyph range: a selection whose ends
     * both scrolled off the same edge covers no visible glyph and is still
     * a selection — Cmd+C must reach it, not fall through to the terminal. */
    return fb->md_sel_ax != fb->md_sel_hx || fb->md_sel_ay != fb->md_sel_hy;
}

/* Cmd+C. The text is NOT produced here: the glyph list is viewport-only, and a
 * selection that runs past either edge of the screen would resolve its
 * off-screen end to the first or last visible glyph, copying only the visible
 * part. The copy is armed and the next frame answers it with a whole-document
 * pass (fb_md_copy_finish); the caller requests that frame. */
bool fb_md_selection_copy(FileBrowser *fb) {
    if (!fb_md_selection_active(fb)) return false;
    fb->md_copy_pending = true;
    return true;
}

/* The copy frame's second half, right after md_render ran with collect_all
 * into `all`/`all_count` — every glyph in the document, framebuffer-placed at
 * the CURRENT scroll. The viewport-only list the highlight reads is rebuilt
 * from the same pass (the rects inside the body, in order), so the next frame
 * draws from a list one frame old instead of an empty one. */
static void fb_md_copy_finish(FileBrowser *fb, const MdGlyphRect *all, u32 all_count,
                              f32 body_y, f32 body_h) {
    fb->md_copy_pending = false;
    char *txt = md_sel_text_from(fb, all, all_count, fb->view_scroll_px);
    if (txt) {
        platform_clipboard_set(txt);
        free(txt);
    }
    u32 n = 0;
    for (u32 i = 0; i < all_count && n < FB_MD_GLYPH_CAP; i++) {
        const MdGlyphRect *g = &all[i];
        if (g->y + g->h <= body_y || g->y >= body_y + body_h) continue;
        fb->md_glyph_rects[n++] = *g;
    }
    fb->md_glyph_count = n;
}

u32 fb_md_outline_count(const FileBrowser *fb) {
    if (!fb || fb->view_mode != FVIEW_MARKDOWN || fb->md_raw_mode) return 0;
    if (!fb->md_outline) return 0;
    return fb->md_outline_count;
}

u8 fb_md_outline_get(const FileBrowser *fb, u32 idx, char *out, usize out_cap) {
    if (out && out_cap) out[0] = '\0';
    if (!fb || !fb->md_outline || idx >= fb->md_outline_count) return 0;
    const MdOutlineItem *it = &fb->md_outline[idx];
    if (out && out_cap) {
        usize n = it->text_len;
        if (n > out_cap - 1) n = out_cap - 1;
        if (it->text && n) memcpy(out, it->text, n);
        out[n] = '\0';
    }
    return it->level ? it->level : 1;
}

void fb_md_scroll_to_heading(FileBrowser *fb, u32 idx) {
    if (!fb || fb->view_mode != FVIEW_MARKDOWN || fb->md_raw_mode) return;
    if (!fb->md_outline || idx >= fb->md_outline_count) return;
    /* Land the heading near the top with a small gap above it. The wheel
     * handler applies the precise content/viewport clamp on the next event;
     * here only negative/over-scroll is avoided. */
    f32 target = fb->md_outline[idx].doc_y - 8.0f;
    if (target < 0.0f) target = 0.0f;
    /* Same ceiling the wheel uses: the end of the document at the bottom of
     * the viewport, not the end of the document at the top of it. */
    f32 max_y = fb->view_content_px - (fb->md_view_h > 0.0f ? fb->md_view_h : 0.0f);
    if (max_y < 0.0f) max_y = 0.0f;
    if (fb->view_content_px > 0.0f && target > max_y) target = max_y;
    fb->view_scroll_px = target;
}

bool fb_md_hit_link_anchored(const FileBrowser *fb, f32 vx, f32 vy,
                             char *out, usize out_cap, u8 *out_kind,
                             char *out_anchor, usize out_anchor_cap,
                             bool *out_anchor_is_block) {
    if (out_anchor && out_anchor_cap) out_anchor[0] = '\0';
    if (out_anchor_is_block) *out_anchor_is_block = false;
    if (!fb || !out || out_cap == 0) return false;
    if (fb->view_mode != FVIEW_MARKDOWN || fb->md_raw_mode) return false;
    if (!fb->md_link_rects || fb->md_link_count == 0) return false;
    for (u32 i = 0; i < fb->md_link_count; i++) {
        const MdLinkRect *L = &fb->md_link_rects[i];
        if (L->kind == MD_LINKRECT_COPY) continue;   /* handled by fb_md_hit_copy */
        if (vx >= L->x && vx < L->x + L->w &&
            vy >= L->y && vy < L->y + L->h) {
            usize n = L->url_len < out_cap - 1 ? L->url_len : out_cap - 1;
            memcpy(out, L->url, n);
            out[n] = '\0';
            if (out_kind) *out_kind = L->kind;
            if (out_anchor && out_anchor_cap && L->anchor && L->anchor_len) {
                usize an = L->anchor_len < out_anchor_cap - 1 ? L->anchor_len
                                                              : out_anchor_cap - 1;
                memcpy(out_anchor, L->anchor, an);
                out_anchor[an] = '\0';
            }
            if (out_anchor_is_block) *out_anchor_is_block = L->anchor_is_block != 0;
            return true;
        }
    }
    return false;
}

bool fb_md_hit_link(const FileBrowser *fb, f32 vx, f32 vy,
                    char *out, usize out_cap, u8 *out_kind) {
    return fb_md_hit_link_anchored(fb, vx, vy, out, out_cap, out_kind,
                                   NULL, 0, NULL);
}

void fb_md_seek_anchor(FileBrowser *fb, const char *anchor, bool is_block) {
    if (!fb || !anchor || !*anchor) return;
    if (!fb->md_doc) {
        if (fb->md_parse_job) {
            snprintf(fb->md_parse_job->seek_anchor, sizeof fb->md_parse_job->seek_anchor,
                     "%s", anchor);
            fb->md_parse_job->seek_is_block = is_block;
        } else if (fb->pending_open_name[0]) {
            snprintf(fb->pending_open_anchor, sizeof fb->pending_open_anchor, "%s", anchor);
            fb->pending_open_anchor_is_block = is_block;
        }
        return;
    }
    u32 first = 0, count = 0;
    if (!md_embed_section(fb->md_doc, (const u8 *)anchor, (u32)strlen(anchor),
                          is_block, &first, &count) || count == 0) {
        /* Not a heading's own text: the anchor a link writes for it
         * (`#whats-new-in-c`, GitHub's rule; md_link_target.h). This is where
         * an in-page link and an open-then-go link both end up. */
        if (is_block) return;
        first = md_link_find_heading(fb->md_doc, anchor, strlen(anchor));
        if (first == MD_LINK_NO_HEADING) return;
    }
    fb->md_seek_block = first;
    fb->md_seek_y     = 0.0f;
}

/* The parser bounds AST nesting. <details> children can borrow original
 * source bytes, while cleaned quote/callout copies carry MD_TASK_NO_SRC. */
static MdBlock *fb_md_task_block(MdBlock *blocks, u32 count, u32 off, u8 state) {
    for (u32 i = 0; i < count; i++) {
        MdBlock *b = &blocks[i];
        if (b->kind == MD_BLOCK_TASK_ITEM && b->src_off == off && b->task_state == state)
            return b;
        MdBlock *child = fb_md_task_block(b->children, b->child_count, off, state);
        if (child) return child;
    }
    return NULL;
}

/* Write the note after a checkbox click, and say so when it did not land. */
static void fb_md_task_persist(FileBrowser *fb) {
    errno = 0;
    bool readonly = false;
    if (fb_write_view_atomic(fb, false, &readonly)) { fb->modified = false; return; }
    int why = errno;
    if (readonly) {
        unsaved_guard_message_readonly(fb->notice, sizeof fb->notice, fb_view_name(fb));
        fb_notice_stamp(fb, true);
    } else if (why) {
        /* A failed write that left errno 0 has no reason to report. */
        fb_notice(fb, true, "Not saved: %s", strerror(why));
    }
}

/* If the source file changed and the view has no local edits, reload the
 * current text before applying the checkbox state. Locate the matching task
 * line in the new text; if it is absent, keep the refreshed content
 * unchanged. */
static void fb_md_task_toggle_fresh(FileBrowser *fb, usize off) {
    const char *old = fb->view_content;
    usize ls = off;
    while (ls > 0 && old[ls - 1] != '\n') ls--;
    usize le = editor_line_break(old, fb->view_size, off);
    usize rel = off - ls, len = le - ls;
    i32 old_line = 0;
    for (usize i = 0; i < ls; i++) old_line += (old[i] == '\n');
    char want = old[off] == ' ' ? 'x' : ' ';
    char *line = malloc(len + 1);
    if (line) { memcpy(line, old + ls, len); line[len] = '\0'; }

    if (!fb_view_reload_now(fb)) {
        free(line);
        fb_notice(fb, true, "%s changed on disk and could not be read again", fb_view_name(fb));
        return;
    }

    /* The line as it was, the state character aside, nearest to its old row. */
    usize hit = (usize)-1;
    i32 best = INT32_MAX, row = 0;
    for (usize at = 0; line && at <= fb->view_size; row++) {
        usize brk = editor_line_break(fb->view_content, fb->view_size, at);
        const char *p = fb->view_content + at;
        if (brk - at == len && rel > 0 && rel + 1 < len &&
            memcmp(p, line, rel) == 0 && memcmp(p + rel + 1, line + rel + 1, len - rel - 1) == 0) {
            i32 away = row > old_line ? row - old_line : old_line - row;
            if (away < best) { best = away; hit = at + rel; }
        }
        if (brk >= fb->view_size) break;
        at = brk + 1;
    }
    free(line);

    if (hit != (usize)-1 && fb->view_content[hit] != want) {
        fb_editor_push_undo(fb);
        if (fb_editor_splice(fb, hit, 1, &want, 1)) {
            fb_editor_push_undo(fb);
            fb_md_reparse(fb);
            /* Said first: a write that fails has the last word. */
            fb_notice(fb, false, "%s changed on disk \xe2\x80\x94 reloaded", fb_view_name(fb));
            fb_md_task_persist(fb);
            return;
        }
    }
    fb_md_reparse(fb);
    fb_notice(fb, false, "%s changed on disk \xe2\x80\x94 reloaded", fb_view_name(fb));
}

/* Hit-test rendered task checkboxes (viewport px, same space as fb_md_hit_link).
 * On a hit, toggle the source "[ ]"<->"[x]" — validated against the live buffer
 * so a stale or recursive (callout/quote) offset can never corrupt the file —
 * then update its task node, mark modified, and persist. Task state affects
 * ink only, so an otherwise-current AST/layout and image jobs stay valid.
 * A missing/stale AST still takes the full parse path.
 *
 * Persisting asks the disk first (FileBrowser.view_disk): the note is written
 * only over the file it was read from. See fb_md_task_toggle_fresh for the
 * file that changed, and the two notices below for the ones it leaves alone. */
bool fb_md_toggle_task_at(FileBrowser *fb, f32 vx, f32 vy) {
    if (!fb || fb->view_mode != FVIEW_MARKDOWN || fb->md_raw_mode) return false;
    if (!fb->md_task_rects || fb->md_task_count == 0 || !fb->view_content) return false;
    if (fb->source != FB_SOURCE_LOCAL) return false;   /* only local files persist */
    for (u32 i = 0; i < fb->md_task_count; i++) {
        const MdTaskRect *T = &fb->md_task_rects[i];
        if (vx < T->x || vx >= T->x + T->w || vy < T->y || vy >= T->y + T->h) continue;
        if (T->src_off == MD_TASK_NO_SRC) return true;   /* not editable */
        usize off = (usize)T->src_off;
        /* Belt and braces: the parser already refuses to hand out an offset it
         * cannot place in the document, and the renderer suppresses rects for
         * transcluded tasks. This last check catches a buffer edited between
         * the render that recorded the rect and this click. */
        if (off == 0 || off + 1 >= fb->view_size) return true;
        char *c = &fb->view_content[off];
        if (c[-1] != '[' || c[1] != ']') return true;
        /* Is the file still the one this note was read from? One stat. */
        FileDiskState disk = file_identity_check(fb->view_path, &fb->view_disk);
        if (disk == FILE_DISK_CHANGED && !fb->modified) {
            fb_md_task_toggle_fresh(fb, off);
            return true;
        }
        MdBlock *task = NULL;
        if (fb->md_doc && !fb->md_source_dirty) {
            task = fb_md_task_block(fb->md_doc->blocks, fb->md_doc->block_count,
                                   (u32)off, *c == ' ' ? 0 : (u8)*c);
        }
        char next = *c == ' ' ? 'x' : ' ';
        uintptr_t source = (uintptr_t)fb->view_content;
        fb_editor_push_undo(fb);
        if (!fb_editor_splice(fb, off, 1, &next, 1)) return true;
        fb_editor_push_undo(fb);
        if (task && source == (uintptr_t)fb->view_content) {
            task->task_state = next == ' ' ? 0 : (u8)next;
            fb->md_source_dirty = false;
        } else {
            fb_md_reparse(fb);
        }
        if (disk == FILE_DISK_CHANGED) {
            /* Edits of this view's own AND a file that moved on. The click
             * stays in the text here and nothing is written: the save is
             * where the two versions meet, and it asks before it overwrites. */
            unsaved_guard_message(fb->notice, sizeof fb->notice, UNSAVED_OVERWRITE,
                                  fb_view_name(fb));
            fb_notice_stamp(fb, true);
        } else if (disk == FILE_DISK_MISSING) {
            /* A click on a checkbox does not bring a deleted file back. */
            fb_notice(fb, true, "%s is no longer on disk \xe2\x80\x94 " UNSAVED_SAVE_KEY
                      " writes it again", fb_view_name(fb));
        } else {
            fb_md_task_persist(fb);
        }
        return true;
    }
    return false;
}

bool fb_md_toggle_fold_at(FileBrowser *fb, f32 vx, f32 vy) {
    if (!fb || fb->view_mode != FVIEW_MARKDOWN || fb->md_raw_mode) return false;
    if (!fb->md_fold_rects || fb->md_fold_count == 0) return false;
    for (u32 i = 0; i < fb->md_fold_count; i++) {
        const MdFoldRect *F = &fb->md_fold_rects[i];
        if (vx < F->x || vx >= F->x + F->w || vy < F->y || vy >= F->y + F->h) continue;
        if (!F->block) return true;
        F->block->fold_open = F->block->fold_open ? 0 : 1;
        F->block->fold_seen = 1;
        /* The block's cached height was measured in the other state. */
        F->block->cached_gen = 0;
        return true;
    }
    return false;
}

/* How long "Copied" stays on the button, and how much of that is the fade.
 * Long enough to read at a glance, short enough that it is gone before the
 * reader looks back — and it is the whole reason a code block can drive frames
 * while nothing else on the page moves, so it is deliberately bounded. */
#define FB_MD_COPY_FLASH_SEC 1.45
#define FB_MD_COPY_FADE_SEC  0.35

f32 fb_md_copy_flash(const FileBrowser *fb, f64 now) {
    if (!fb || !fb->md_copy_flash_src || fb->md_copy_flash_at <= 0.0) return 0.0f;
    f64 age = now - fb->md_copy_flash_at;
    if (age < 0.0) return 1.0f;                       /* clock stepped back */
    if (age >= FB_MD_COPY_FLASH_SEC) return 0.0f;
    f64 left = FB_MD_COPY_FLASH_SEC - age;
    if (left >= FB_MD_COPY_FADE_SEC) return 1.0f;
    return (f32)(left / FB_MD_COPY_FADE_SEC);
}

bool fb_md_hit_copy(const FileBrowser *fb, f32 vx, f32 vy,
                    const u8 **out_text, u32 *out_len) {
    if (!fb || fb->view_mode != FVIEW_MARKDOWN || fb->md_raw_mode) return false;
    if (!fb->md_link_rects || fb->md_link_count == 0) return false;
    for (u32 i = 0; i < fb->md_link_count; i++) {
        const MdLinkRect *L = &fb->md_link_rects[i];
        if (L->kind != MD_LINKRECT_COPY) continue;
        if (vx >= L->x && vx < L->x + L->w &&
            vy >= L->y && vy < L->y + L->h) {
            if (out_text) *out_text = L->url;
            if (out_len)  *out_len  = L->url_len;
            return true;
        }
    }
    return false;
}

f32 fb_view_row_px(const FileBrowser *fb) {
    if (!fb) return 0.0f;
    if (fb->view_mode == FVIEW_MARKDOWN && fb->md_raw_mode)
        return fb->live_ch > 0.0f ? md_live_line_h(0, fb->live_ch, fb->live_dpi) : 0.0f;
    if (fb->view_mode == FVIEW_CODE || fb->view_mode == FVIEW_TEXT)
        return fb->code_ch;
    return 0.0f;
}

/* Accumulate fractional trackpad deltas until they reach one line-height before
 * changing the line-based scroll offset. Do not round each event independently.
 */
void fb_scroll_lines(FileBrowser *fb, f32 dy, bool precise, f32 fallback_row_px) {
    if (!fb) return;
    f32 row_px = fb_view_row_px(fb);
    if (row_px < 1.0f) row_px = fallback_row_px;
    if (fb_md_edit_active(fb) && fb->live_width > 0.0f) {
        f32 delta = precise ? dy : dy * row_px * PLATFORM_VIEW_WHEEL_LINES;
        fb->view_scroll_px = fmaxf(0.0f, fminf(fb->view_scroll_px - delta,
                                             fb->view_content_px - fb->ed_area_h));
        fb->live_follow = false;
        return;
    }
    i32 step = line_scroll_take(&fb->vscroll_accum_px, dy, precise,
                                row_px, PLATFORM_VIEW_WHEEL_LINES);
    fb->view_scroll -= step;
    i32 max_scroll = fb->view_line_count > 10 ? fb->view_line_count - 10 : 0;
    if (fb->view_scroll < 0) fb->view_scroll = 0;
    if (fb->view_scroll > max_scroll) fb->view_scroll = max_scroll;
    /* A swipe that pushes against either end owes nothing: carrying it would
     * make the first pixels back the other way pay for it. */
    if (fb->view_scroll == 0 && fb->vscroll_accum_px > 0.0f) fb->vscroll_accum_px = 0.0f;
    if (fb->view_scroll == max_scroll && fb->vscroll_accum_px < 0.0f) fb->vscroll_accum_px = 0.0f;
}

void fb_handle_scroll(FileBrowser *fb, f32 dy, bool precise) {
    /* macOS NSEvent.scrollingDeltaY semantics:
     *   precise=true  → pixel delta (trackpad / Magic Mouse), already in
     *                   pixels and used as is.
     *   precise=false → line-unit delta (legacy wheel); 22 px ≈ one entry row,
     *                   so one click moves ~one entry. */
    f32 dy_px = precise ? dy : (dy * 22.0f);
    fb->scroll_offset_px -= dy_px;

    /* Clamp to valid range */
    if (fb->scroll_offset_px < 0) fb->scroll_offset_px = 0;
    f32 max_scroll = fb->content_height - fb->viewport_height;
    if (max_scroll < 0) max_scroll = 0;
    if (fb->scroll_offset_px > max_scroll) fb->scroll_offset_px = max_scroll;

    /* Keep the legacy integer offset in sync, in rows of the height the list
     * was last drawn at. */
    f32 entry_h = fb->list_row_h > 0.0f ? fb->list_row_h : fb_list_row_h(1.0f);
    fb->scroll_offset = (i32)(fb->scroll_offset_px / entry_h);
    if (fb->scroll_offset < 0) fb->scroll_offset = 0;
    if (fb->scroll_offset >= fb->entry_count) fb->scroll_offset = fb->entry_count - 1;
    if (fb->scroll_offset < 0) fb->scroll_offset = 0;
}
