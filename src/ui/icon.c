/*
 * Liu — unified UI icon implementation. See icon.h for the rationale.
 */
#include "ui/icon.h"
#include "platform/platform.h"

#include <math.h>
#include <stddef.h>

/* SF Symbol name for each kind. NULL = no symbol, fallback only. */
static const char *icon_symbol_name(IconKind k) {
    switch (k) {
    /* Sidebar is custom-drawn (draw_sidebar) for a symmetric, centred divider —
     * the SF "sidebar.left" symbol sits its divider ~1/3 from the left, which
     * reads as lopsided in the toolbar. */
    case ICON_GEAR:           return "gearshape";
    case ICON_NETWORK:        return "network";
    case ICON_FONT_LARGER:    return "textformat.size.larger";
    case ICON_FONT_SMALLER:   return "textformat.size.smaller";
    case ICON_CLOSE:          return "xmark";
    case ICON_PLUS:           return "plus";
    case ICON_MINUS:          return "minus";
    case ICON_SEARCH:         return "magnifyingglass";
    case ICON_UP:             return "arrow.up";
    case ICON_REFRESH:        return "arrow.clockwise";
    case ICON_CHEVRON_DOWN:   return "chevron.down";
    case ICON_CHEVRON_RIGHT:  return "chevron.right";
    case ICON_BELL:           return "bell.fill";
    case ICON_MOON:           return "moon.fill";
    case ICON_CHECK:          return "checkmark";
    default:                  return NULL;
    }
}

/* Vector-fallback draw helpers. Drawn with renderer_draw_rect since we
 * can't rely on the SDF pipeline's path support — keep shapes simple and
 * monoline so they read cleanly at 14–18 dpi. */

static void draw_xmark(Renderer *r, f32 x, f32 y, f32 sz, Color c) {
    /* Two rotated bars approximated with a stack of stepped rects. The
     * stair-stepping is fine at small sizes since the 2 px stroke
     * dominates the perceived shape. */
    f32 inset  = sz * 0.22f;
    f32 stroke = fmaxf(1.5f, sz * 0.10f);
    f32 a      = inset;
    f32 b      = sz - inset;
    f32 steps  = sz - 2 * inset;
    if (steps < 1.0f) steps = 1.0f;
    for (f32 t = 0.0f; t <= steps; t += 1.0f) {
        f32 dx = a + t;
        f32 dy = a + t;
        renderer_draw_rect(r, x + dx, y + dy, stroke, stroke, c);
        renderer_draw_rect(r, x + (b - t), y + dy, stroke, stroke, c);
    }
}

static void draw_hbar(Renderer *r, f32 x, f32 y, f32 sz, Color c) {
    f32 inset  = sz * 0.20f;
    f32 stroke = fmaxf(1.5f, sz * 0.10f);
    renderer_draw_rect(r, x + inset, y + (sz - stroke) * 0.5f,
                       sz - 2 * inset, stroke, c);
}

static void draw_plus(Renderer *r, f32 x, f32 y, f32 sz, Color c) {
    f32 inset  = sz * 0.20f;
    f32 stroke = fmaxf(1.5f, sz * 0.10f);
    /* Horizontal bar */
    renderer_draw_rect(r, x + inset, y + (sz - stroke) * 0.5f,
                       sz - 2 * inset, stroke, c);
    /* Vertical bar */
    renderer_draw_rect(r, x + (sz - stroke) * 0.5f, y + inset,
                       stroke, sz - 2 * inset, c);
}

static void draw_arrow_up(Renderer *r, f32 x, f32 y, f32 sz, Color c) {
    f32 inset  = sz * 0.18f;
    f32 stroke = fmaxf(1.5f, sz * 0.10f);
    /* Vertical shaft */
    renderer_draw_rect(r, x + (sz - stroke) * 0.5f, y + inset,
                       stroke, sz - 2 * inset, c);
    /* Diagonal head — staircase */
    f32 head = sz * 0.30f;
    for (f32 t = 0; t < head; t += 1.0f) {
        renderer_draw_rect(r, x + (sz - stroke) * 0.5f - t,
                           y + inset + t, stroke, stroke, c);
        renderer_draw_rect(r, x + (sz - stroke) * 0.5f + t,
                           y + inset + t, stroke, stroke, c);
    }
}

static void draw_chevron(Renderer *r, f32 x, f32 y, f32 sz, Color c, bool right) {
    f32 stroke = fmaxf(1.5f, sz * 0.12f);
    f32 mid    = sz * 0.5f;
    f32 reach  = sz * 0.30f;
    if (right) {
        /* > shape: two diagonals meeting on the right edge. */
        for (f32 t = 0; t < reach; t += 1.0f) {
            renderer_draw_rect(r, x + mid - reach + t, y + mid - reach + t,
                               stroke, stroke, c);
            renderer_draw_rect(r, x + mid - reach + t, y + mid + reach - t,
                               stroke, stroke, c);
        }
    } else {
        /* v shape: two diagonals meeting at the bottom. */
        for (f32 t = 0; t < reach; t += 1.0f) {
            renderer_draw_rect(r, x + mid - reach + t, y + mid - reach + t,
                               stroke, stroke, c);
            renderer_draw_rect(r, x + mid + reach - t, y + mid - reach + t,
                               stroke, stroke, c);
        }
    }
}

static void draw_circle_fill(Renderer *r, f32 x, f32 y, f32 sz, Color c) {
    /* Pixel-stepped filled circle — replaces missing SF symbol at small
     * sizes for bell.fill / moon.fill / checkmark backdrop. Centered. */
    f32 cx = x + sz * 0.5f;
    f32 cy = y + sz * 0.5f;
    f32 rad = sz * 0.40f;
    i32 steps = (i32)(rad * 2.0f);
    for (i32 i = 0; i < steps; i++) {
        f32 dy = (f32)i - rad;
        f32 dx = sqrtf(fmaxf(0.0f, rad * rad - dy * dy));
        renderer_draw_rect(r, cx - dx, cy + dy, dx * 2.0f, 1.0f, c);
    }
}

static void draw_sidebar(Renderer *r, f32 x, f32 y, f32 sz, Color c) {
    /* Outlined rectangle with a CENTRED vertical divider and a lightly-filled
     * left compartment, so it reads as a sidebar panel while keeping the
     * left/right gaps symmetric. Flat rects only (square corners read crisp at
     * toolbar sizes and stay inside the flat→rrect→glyph flush order). */
    f32 inset = floorf(sz * 0.17f);
    f32 bx = floorf(x + inset), by = floorf(y + inset);
    f32 bw = floorf(sz - 2.0f * inset), bh = floorf(sz - 2.0f * inset);
    if (bw < 3.0f || bh < 3.0f) { draw_circle_fill(r, x, y, sz, c); return; }
    f32 st = fmaxf(1.0f, floorf(sz * 0.07f + 0.5f));   /* stroke weight */
    f32 cx = floorf(bx + bw * 0.5f);                   /* centred divider */

    /* Left compartment fill — subtle so the outline + divider stay dominant. */
    Color fill = c; fill.a *= 0.28f;
    renderer_draw_rect(r, bx, by, cx - bx, bh, fill);

    /* Outer outline. */
    renderer_draw_rect(r, bx, by, bw, st, c);                 /* top */
    renderer_draw_rect(r, bx, by + bh - st, bw, st, c);       /* bottom */
    renderer_draw_rect(r, bx, by, st, bh, c);                 /* left */
    renderer_draw_rect(r, bx + bw - st, by, st, bh, c);       /* right */

    /* Centred divider. */
    renderer_draw_rect(r, cx - st * 0.5f, by, st, bh, c);
}

static void draw_fallback(Renderer *r, IconKind kind,
                          f32 x, f32 y, f32 sz, Color c) {
    switch (kind) {
    case ICON_SIDEBAR:       draw_sidebar(r, x, y, sz, c); return;
    case ICON_CLOSE:         draw_xmark(r, x, y, sz, c); return;
    case ICON_MINUS:         draw_hbar(r, x, y, sz, c);  return;
    case ICON_PLUS:
    case ICON_FONT_LARGER:   draw_plus(r, x, y, sz, c);  return;
    case ICON_FONT_SMALLER:  draw_hbar(r, x, y, sz, c);  return;
    case ICON_UP:            draw_arrow_up(r, x, y, sz, c); return;
    case ICON_CHEVRON_RIGHT: draw_chevron(r, x, y, sz, c, true);  return;
    case ICON_CHEVRON_DOWN:  draw_chevron(r, x, y, sz, c, false); return;
    case ICON_BELL:
    case ICON_MOON:
    case ICON_CHECK:         draw_circle_fill(r, x, y, sz, c); return;
    /* SIDEBAR / GEAR / NETWORK / SEARCH / REFRESH have no good
     * geometry-only fallback — render a small dot so the click target
     * is still visually present. */
    default:                 draw_circle_fill(r, x, y, sz, c); return;
    }
}

void icon_draw(Renderer *r, IconKind kind, f32 x, f32 y, f32 size, Color color) {
    if (!r || kind == ICON_NONE || size < 1.0f) return;

    /* Pixel-snap origin so SF Symbol bitmaps don't get bilinearly blurred
     * across two framebuffer pixels — the supersampled raster only looks
     * crisp when its quad starts on an integer column. */
    f32 px = floorf(x + 0.5f);
    f32 py = floorf(y + 0.5f);
    f32 ps = floorf(size + 0.5f);

    const char *symbol = icon_symbol_name(kind);
    if (symbol) {
        const u8 *pixels = NULL;
        i32 img_w = 0, img_h = 0;
        u8 cr = (u8)fminf(fmaxf(color.r * 255.0f, 0.0f), 255.0f);
        u8 cg = (u8)fminf(fmaxf(color.g * 255.0f, 0.0f), 255.0f);
        u8 cb = (u8)fminf(fmaxf(color.b * 255.0f, 0.0f), 255.0f);
        u8 ca = (u8)fminf(fmaxf(color.a * 255.0f, 0.0f), 255.0f);
        if (platform_get_system_symbol_rgba(symbol, (i32)ps,
                                            cr, cg, cb, ca,
                                            &pixels, &img_w, &img_h) &&
            pixels && img_w > 0 && img_h > 0) {
            /* Key the GPU image cache by (kind, size) + a colour generation —
             * NOT by the rasterizer's pixel pointer. The SF-symbol cache freely
             * frees and reuses those buffers, so a reused address aliased to a
             * previously-uploaded texture and the GPU handed back the WRONG
             * icon (the "sidebar icons keep breaking" scramble). A stable
             * per-(kind,size) anchor address makes the cache identify icons by
             * what they ARE; the colour packs into the generation so a
             * re-tinted icon re-uploads in place instead of colliding. */
            #define ICON_KEY_PX 256
            static const char key_anchor[ICON_COUNT][ICON_KEY_PX];
            i32 sidx = (i32)ps;
            if (sidx < 0) sidx = 0;
            if (sidx >= ICON_KEY_PX) sidx = ICON_KEY_PX - 1;
            const void *key = &key_anchor[kind][sidx];
            u64 gen = ((u64)cr << 24) | ((u64)cg << 16) | ((u64)cb << 8) | (u64)ca;
            renderer_draw_image_cached(r, key, gen, pixels, img_w, img_h,
                                       px, py, ps, ps);
            return;
        }
    }

    draw_fallback(r, kind, px, py, ps, color);
}
