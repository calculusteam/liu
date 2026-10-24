/*
 * Capture effective runtime settings as one JSON snapshot so performance
 * reports can be interpreted against the configuration the application
 * actually loaded.
 */
#ifndef UI_EFFECTIVE_SETTINGS_H
#define UI_EFFECTIVE_SETTINGS_H

#include "core/types.h"

typedef struct {
    /* What was loaded. `font_path` is the face the atlas was BUILT from —
     * after the fallback ladder, not the one config.json named. */
    char font_path[512];
    f32  font_size;          /* points */
    f32  font_weight;
    f32  line_height;
    bool font_normalize;
    char theme[64];

    /* What it produced. */
    f32  dpi_scale;
    f32  cell_w_px, cell_h_px;
    i32  grid_cols, grid_rows;   /* the focused terminal; 0 when there is none */
    i32  window_w_pt, window_h_pt;
    i32  fb_w_px, fb_h_px;
    f32  panel_hz;               /* 0 when the platform cannot say */

    /* What decides the idle and compositor cost. */
    bool cursor_blink;
    f32  opacity;
    bool background_blur;
    f32  background_blur_radius;

    /* Provenance. */
    char backend[16];            /* "metal" / "opengl" */
    char build[16];              /* "debug" / "release" */
    bool e2e_probe;              /* built with LIU_E2E_PROBE */
    char config_dir[512];        /* where config.json was read from */
    i32  presents_before;        /* frames presented before the snapshot */
} EffectiveSettings;

/* Format s as a JSON object with one key per line in declaration order and
 * escaped strings. Return 0 when cap is too small; callers must treat the
 * snapshot as unavailable rather than use a truncated result. */
usize effective_settings_format(const EffectiveSettings *s, char *out, usize cap);

/* Write the snapshot to path through a temporary file and rename it over
 * the destination, so readers see either the previous file or the complete
 * new snapshot. Return false on failure. */
bool effective_settings_write(const EffectiveSettings *s, const char *path);

#endif /* UI_EFFECTIVE_SETTINGS_H */
