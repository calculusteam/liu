/*
 * Liu — the scrollback scrollbar's geometry, in one place.
 *
 * The renderer paints the bar, the hit test decides what a press on it means
 * and the drag handler turns pointer motion back into a scroll offset — three
 * sites that must agree on the bar's rectangle, the git-diff dock's reserve
 * included (or the grab zone would sit under the panel while the bar is drawn
 * beside it). Pure arithmetic, no AppState, so the round trip can be checked
 * without a window.
 *
 * Conventions: framebuffer pixels; `scroll_offset` counts lines scrolled UP
 * into history (0 = live bottom), so the thumb sits at the bottom of the
 * track at offset 0 and at the top at offset == sb_count.
 */
#ifndef UI_SCROLLBAR_GEOM_H
#define UI_SCROLLBAR_GEOM_H

#include "core/types.h"
#include <stdbool.h>

/* The painted pill is narrower than the grab zone: the zone is the layout's
 * SCROLLBAR_W_PT column, the pill rests at SB_PILL_REST_PT and grows to the
 * full column while the pointer is over it or the thumb is being dragged.
 * Every pane of a split carries its own bar, so the resting pill is a hairline:
 * N of them must not read as N slabs. */
#define SB_PILL_REST_PT    3.0f
#define SB_THUMB_MIN_PT   24.0f
#define SB_TRACK_INSET_PT  2.0f   /* the track stops short of the band's ends */

typedef struct {
    f32 zone_x, zone_w;          /* the grab column (hit test) */
    f32 track_x, track_y, track_w, track_h;   /* the painted pill track */
    f32 thumb_y, thumb_h;        /* the thumb, inside the track */
    bool valid;                  /* false when there is nothing to scroll */
} ScrollbarGeom;

/* `hot` in 0..1 widens the pill from its resting width toward the full zone.
 * `band_top/bottom` is the terminal body's vertical span (ui_terminal_vspan).
 * `zone_right` is the RIGHT edge of the grab column: the right edge of the
 * pane the bar belongs to (ui_terminal_pane_scrollbar), so every pane of a
 * split carries its own bar beside its own grid. */
static inline ScrollbarGeom scrollbar_geom_at(f32 zone_right, f32 zone_w,
                                              f32 band_top, f32 band_bottom,
                                              f32 dpi, i32 rows, i32 sb_count,
                                              i32 scroll_offset, f32 hot) {
    ScrollbarGeom g;
    g.zone_w  = zone_w;
    g.zone_x  = zone_right - zone_w;
    f32 inset = SB_TRACK_INSET_PT * dpi;
    g.track_y = band_top + inset;
    g.track_h = (band_bottom - band_top) - 2.0f * inset;
    if (hot < 0.0f) hot = 0.0f;
    if (hot > 1.0f) hot = 1.0f;
    f32 rest = SB_PILL_REST_PT * dpi;
    if (rest > zone_w) rest = zone_w;
    g.track_w = rest + (zone_w - rest) * hot;
    /* Grows toward the left: the right edge stays put against the pane's edge. */
    g.track_x = g.zone_x + zone_w - g.track_w;
    g.valid = rows > 0 && sb_count > 0 && g.track_h > 0.0f;
    if (!g.valid) { g.thumb_y = g.track_y; g.thumb_h = 0.0f; return g; }
    i32 total = rows + sb_count;
    f32 thumb_h = g.track_h * ((f32)rows / (f32)total);
    f32 min_h = SB_THUMB_MIN_PT * dpi;
    if (thumb_h < min_h) thumb_h = min_h;
    if (thumb_h > g.track_h) thumb_h = g.track_h;
    f32 ratio = (f32)scroll_offset / (f32)sb_count;
    if (ratio < 0.0f) ratio = 0.0f;
    if (ratio > 1.0f) ratio = 1.0f;
    g.thumb_h = thumb_h;
    g.thumb_y = g.track_y + (g.track_h - thumb_h) * (1.0f - ratio);
    return g;
}

/* The window-edge form: the column beside the window's content box, right of
 * the padding and the dock/viewer reserve. A lone pane's bar is exactly this. */
static inline ScrollbarGeom scrollbar_geom(f32 fb_width, f32 padding, f32 right_reserve,
                                           f32 zone_w, f32 band_top, f32 band_bottom,
                                           f32 dpi, i32 rows, i32 sb_count,
                                           i32 scroll_offset, f32 hot) {
    return scrollbar_geom_at(fb_width - padding - right_reserve, zone_w, band_top,
                             band_bottom, dpi, rows, sb_count, scroll_offset, hot);
}

/* The offset a thumb whose TOP sits at `thumb_top` stands for — the inverse
 * of the placement above, used by the drag. Clamped to [0, sb_count]. */
static inline i32 scrollbar_offset_for_thumb_top(const ScrollbarGeom *g, i32 sb_count,
                                                 f32 thumb_top) {
    f32 range = g->track_h - g->thumb_h;
    if (!g->valid || sb_count <= 0 || range <= 0.0f) return 0;
    f32 ratio = 1.0f - (thumb_top - g->track_y) / range;
    if (ratio < 0.0f) ratio = 0.0f;
    if (ratio > 1.0f) ratio = 1.0f;
    i32 off = (i32)(ratio * (f32)sb_count + 0.5f);
    if (off < 0) off = 0;
    if (off > sb_count) off = sb_count;
    return off;
}

/* A press in the zone: on the thumb it starts a drag (returns true and the
 * grab offset from the thumb's top); beside it the thumb jumps so its CENTRE
 * lands under the pointer, macOS-style, and the drag continues from there. */
static inline bool scrollbar_press(const ScrollbarGeom *g, i32 sb_count, f32 y,
                                   i32 *out_offset, f32 *out_grab) {
    if (!g->valid) return false;
    if (y >= g->thumb_y && y < g->thumb_y + g->thumb_h) {
        *out_offset = -1;                      /* unchanged */
        *out_grab   = y - g->thumb_y;
        return true;
    }
    *out_grab   = g->thumb_h * 0.5f;
    *out_offset = scrollbar_offset_for_thumb_top(g, sb_count, y - *out_grab);
    return true;
}

#endif /* UI_SCROLLBAR_GEOM_H */
