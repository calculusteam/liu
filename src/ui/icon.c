/*
 * Liu — unified UI icon implementation. See icon.h for the rationale.
 */
#include "ui/icon.h"
#include "ui/lucide.h"
#include "platform/platform.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>

/* IconKind -> Lucide icon id.
 *
 * Designated initialisers make the row's position BE its kind, so an entry
 * can't drift out of order the way a hand-sorted array can — the compiler
 * places it. The _Static_assert then pins the table's length to ICON_COUNT, so
 * adding a kind without a row (or a row without a kind) fails the build rather
 * than shipping a blank icon. Whether every id actually resolves to an embedded
 * SVG is checked by icon_table_selfcheck() below — that part needs the data,
 * not the type system.
 *
 * These are real vector paths rasterised at draw size, so curves are curves at
 * every DPI instead of staircases. */
static const char *const ICON_LUCIDE[ICON_COUNT] = {
    [ICON_NONE]           = NULL,
    [ICON_SIDEBAR]        = "panel-left",
    [ICON_GEAR]           = "settings",
    [ICON_NETWORK]        = "globe",
    [ICON_FONT_LARGER]    = "a-arrow-up",
    [ICON_FONT_SMALLER]   = "a-arrow-down",
    [ICON_CLOSE]          = "x",
    [ICON_PLUS]           = "plus",
    [ICON_MINUS]          = "minus",
    [ICON_SEARCH]         = "search",
    [ICON_UP]             = "arrow-up",
    [ICON_REFRESH]        = "refresh-cw",
    [ICON_CHEVRON_DOWN]   = "chevron-down",
    [ICON_CHEVRON_RIGHT]  = "chevron-right",
    [ICON_CHEVRON_LEFT]   = "chevron-left",
    [ICON_ARROW_LEFT]     = "arrow-left",
    [ICON_ARROW_RIGHT]    = "arrow-right",
    [ICON_ARROW_UP_RIGHT] = "arrow-up-right",
    [ICON_SCOPE]          = "crosshair",
    [ICON_DEVICE]         = "smartphone",
    [ICON_STOP]           = "square",
    [ICON_BELL]           = "bell",
    [ICON_MOON]           = "moon",
    [ICON_CHECK]          = "check",
    [ICON_TOGGLE]         = "toggle-left",
    [ICON_EYE]            = "eye",
    [ICON_PALETTE]        = "palette",
    [ICON_KEYBOARD]       = "keyboard",
    [ICON_INFO]           = "info",
    [ICON_LANGUAGES]      = "languages",
    [ICON_MIC]            = "mic",
    [ICON_COPY]           = "copy",
    [ICON_PENCIL]         = "pencil",
    [ICON_PANEL_RIGHT]    = "panel-right",
    [ICON_PANEL_LEFT]     = "panel-left",
    [ICON_PANEL_BOTTOM]   = "panel-bottom",
    [ICON_PANEL_TOP]      = "panel-top",
    [ICON_GRAPH]          = "waypoints",
    [ICON_PROMPT]         = "message-square-quote",
    [ICON_TERMINAL]       = "terminal",
    [ICON_FT_FOLDER]      = "folder",
    [ICON_FT_DOC]         = "file",
    [ICON_FT_DOC_TEXT]    = "file-text",
    [ICON_FT_CODE]        = "file-code",
    [ICON_FT_IMAGE]       = "file-image",
    [ICON_FT_FILM]        = "film",
    [ICON_FT_MUSIC]       = "file-music",
    [ICON_FT_ARCHIVE]     = "file-archive",
    [ICON_FT_PDF]         = "file-type",
    [ICON_FT_TERMINAL]    = "terminal",
    [ICON_FT_BRANCH]      = "git-branch",
    [ICON_FT_LOCK]        = "lock",
    [ICON_SLIDERS]        = "sliders-horizontal",
    [ICON_BOT]            = "bot",
    [ICON_NOTEBOOK]       = "notebook",
    [ICON_GRIP]           = "grip-vertical",
    [ICON_LINE]           = "slash",
    [ICON_SQUARE]         = "square",
    [ICON_ERASER]         = "eraser",
    [ICON_UNDO]           = "undo-2",
    [ICON_REDO]           = "redo-2",
    [ICON_TRASH]          = "trash-2",
    [ICON_COMMAND]        = "command",
    [ICON_SHIFT]          = "shift",
    [ICON_OPTION]         = "option",
    [ICON_CONTROL]        = "control",
    [ICON_DOWN]           = "arrow-down",
    [ICON_SCISSORS]       = "scissors",
    [ICON_CLIPBOARD]      = "clipboard",
    [ICON_FILE_PLUS]      = "file-plus",
    [ICON_FOLDER_PLUS]    = "folder-plus",
    [ICON_ALERT]          = "triangle-alert",
    [ICON_CIRCLE_X]       = "circle-x",
    [ICON_LAPTOP]         = "laptop",
    [ICON_SERVER]         = "server",
    [ICON_FILTER]         = "list-filter",
    [ICON_UPLOAD]         = "upload",
    [ICON_DOWNLOAD]       = "download",
    [ICON_HOME]           = "house",
    [ICON_EYE_OFF]        = "eye-off",
    [ICON_CHEVRON_UP]     = "chevron-up",
    [ICON_FOLDER_SYNC]    = "folder-sync",
    [ICON_WINDOW]         = "app-window",
    [ICON_COLUMNS]        = "columns-2",
    [ICON_ROWS]           = "rows-2",
    [ICON_GAUGE]          = "gauge",
    [ICON_SHAPES]         = "shapes",
    [ICON_ROCKET]         = "rocket",
    [ICON_GROUP]          = "group",
    [ICON_TYPE]           = "type",
    [ICON_BLEND]          = "blend",
    [ICON_USER]           = "user",
    [ICON_TEXT_CURSOR]    = "text-cursor",
    [ICON_SCROLL]         = "scroll-text",
    [ICON_KEY]            = "key-round",
    [ICON_FINGERPRINT]    = "fingerprint",
    [ICON_ARROW_LEFT_RIGHT] = "arrow-left-right",
    [ICON_BROADCAST]      = "radio-tower",
    [ICON_HASH]           = "hash",
    [ICON_HISTORY]        = "history",
    [ICON_BRUSH]          = "brush",
};
_Static_assert(sizeof(ICON_LUCIDE) / sizeof(ICON_LUCIDE[0]) == ICON_COUNT,
               "ICON_LUCIDE must have exactly one row per IconKind");

static const char *icon_lucide_name(IconKind k) {
    if (k <= ICON_NONE || k >= ICON_COUNT) return NULL;
    return ICON_LUCIDE[k];
}

i32 icon_table_selfcheck(const char **missing_kind_name) {
    if (missing_kind_name) *missing_kind_name = NULL;
    i32 bad = 0;
    for (i32 k = ICON_NONE + 1; k < ICON_COUNT; k++) {
        const char *name = ICON_LUCIDE[k];
        if (!name || !name[0] || !lucide_icon_exists(name)) {
            if (bad == 0 && missing_kind_name) *missing_kind_name = name;
            bad++;
        }
    }
    return bad;
}

void icon_ink_insets(IconKind kind, f32 size, f32 *left, f32 *right) {
    if (left)  *left  = 0.0f;
    if (right) *right = 0.0f;
    const char *name = icon_lucide_name(kind);
    if (!name || size < 1.0f) return;
    lucide_icon_ink_insets(name, (i32)floorf(size + 0.5f), left, right);
}

void icon_draw(Renderer *r, IconKind kind, f32 x, f32 y, f32 size, Color color) {
    if (!r || kind == ICON_NONE || size < 1.0f) return;
    const char *name = icon_lucide_name(kind);
    if (!name) return;

    /* Pixel-snap the quad: the raster is produced at exactly this pixel size,
     * so a fractional origin would resample it and undo that. */
    f32 px = floorf(x + 0.5f);
    f32 py = floorf(y + 0.5f);
    f32 ps = floorf(size + 0.5f);

    /* Upload the WHITE mask and tint in the shader — never a pre-tinted
     * bitmap. The tinted variant would make the texture's contents depend on
     * the colour, and the image cache keys by (kind, size): a second draw of
     * the same icon in a different colour would re-upload over the SAME
     * texture while the first draw is merely encoded, not yet executed, so
     * both would come out in whichever colour was written last. Colour lives
     * in the draw call, so the upload is colour-independent. */
    const u8 *pixels = lucide_icon_rgba(name, (i32)ps);
    if (!pixels) return;

    /* Key the GPU image cache by icon kind and size, not by the pixel pointer.
     * Raster buffers are freed and reused, so a recycled address can alias an
     * earlier texture. For a given kind and size the mask is constant,
     * allowing the uploaded texture to remain cached. */
    #define ICON_KEY_PX 256
    static const char key_anchor[ICON_COUNT][ICON_KEY_PX];
    i32 sidx = (i32)ps;
    if (sidx < 0) sidx = 0;
    if (sidx >= ICON_KEY_PX) sidx = ICON_KEY_PX - 1;
    const void *key = &key_anchor[kind][sidx];
    renderer_draw_image_tinted(r, key, 0, pixels, (i32)ps, (i32)ps,
                               px, py, ps, ps, color);
    #undef ICON_KEY_PX
}

/* =========================================================================
 * Shared icon resampler (see icon.h for the rationale)
 * ========================================================================= */

u8 *ui_icon_downscale_rgba(const u8 *src, i32 sw, i32 sh, i32 dw, i32 dh) {
    if (!src || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0 ||
        dw > sw || dh > sh) return NULL;
    u8 *dst = malloc((usize)dw * (usize)dh * 4u);
    if (!dst) return NULL;

    f32 rx = (f32)sw / (f32)dw;
    f32 ry = (f32)sh / (f32)dh;
    for (i32 dy = 0; dy < dh; dy++) {
        f32 y0 = (f32)dy * ry, y1 = (f32)(dy + 1) * ry;
        i32 iy0 = (i32)y0;
        i32 iy1 = (i32)ceilf(y1); if (iy1 > sh) iy1 = sh;
        for (i32 dx = 0; dx < dw; dx++) {
            f32 x0 = (f32)dx * rx, x1 = (f32)(dx + 1) * rx;
            i32 ix0 = (i32)x0;
            i32 ix1 = (i32)ceilf(x1); if (ix1 > sw) ix1 = sw;
            /* Integrate the exact source footprint of this dest pixel; edge
             * texels contribute their covered fraction only.
             *
             * The source arrives already premultiplied (every producer runs
             * img_premultiply_rgba at decode), so this is a straight weighted
             * average of all four channels — no per-texel alpha scaling, and
             * crucially no unpremultiply on the way out: the image blend is
             * (ONE, 1-SrcAlpha) and consumes premultiplied. */
            f64 r = 0.0, g = 0.0, b = 0.0, a = 0.0, area = 0.0;
            for (i32 sy = iy0; sy < iy1; sy++) {
                f32 wy = fminf((f32)(sy + 1), y1) - fmaxf((f32)sy, y0);
                const u8 *row = src + ((usize)sy * (usize)sw) * 4u;
                for (i32 sx = ix0; sx < ix1; sx++) {
                    f32 wx = fminf((f32)(sx + 1), x1) - fmaxf((f32)sx, x0);
                    f64 wgt = (f64)wx * (f64)wy;
                    const u8 *p = row + (usize)sx * 4u;
                    r += (f64)p[0] * wgt;
                    g += (f64)p[1] * wgt;
                    b += (f64)p[2] * wgt;
                    a += (f64)p[3] * wgt;
                    area += wgt;
                }
            }
            u8 *q = dst + ((usize)dy * (usize)dw + (usize)dx) * 4u;
            if (area > 0.0) {
                f64 inv = 1.0 / area;
                f64 o[4] = { r * inv, g * inv, b * inv, a * inv };
                for (i32 c = 0; c < 4; c++) {
                    f64 v = o[c] + 0.5;
                    if (v < 0.0) v = 0.0;
                    if (v > 255.0) v = 255.0;
                    q[c] = (u8)v;
                }
            } else {
                q[0] = q[1] = q[2] = q[3] = 0;
            }
        }
    }
    return dst;
}
