/*
 * Liu - GPU terminal renderer (multi-pass)
 *
 * Rendering pipeline (back-to-front):
 *   Pass 0: glClear with terminal background color
 *   Pass 1: Cell backgrounds — only cells with non-default bg (rect batch)
 *   Pass 2: Cell glyphs — instanced alpha-blended glyph quads (single draw call)
 *   Pass 3: Decorations — cursor, underline, strikethrough, selection (rect batch)
 */
#ifndef RENDERER_H
#define RENDERER_H

#include "core/types.h"
#include <math.h>

/* =========================================================================
 * Color pipeline
 *
 * Invariant: on the CPU side, every `Color` value is in the **sRGB** gamma
 * space (0..1 floats matching hex-picker/CSS RGB values). Arithmetic such
 * as DIM / BOLD / selection alpha happens in sRGB with compensated
 * coefficients.
 *
 * The conversion to linear space happens exactly once — at the CPU → GPU
 * boundary — inside:
 *    renderer_draw_rect()    renderer_push_glyph()
 *    renderer_draw_glyph()   renderer_set_clear_color()
 *    renderer_compute_terminal()   (palette upload)
 *
 * Per-row render caches store already-linear colors so cached rows can be
 * replayed with memcpy and no per-frame gamma work.
 *
 * GPU framebuffer attachments are `_sRGB`-tagged, so the hardware applies
 * linear → sRGB encode on write and sRGB → linear decode on sampled-from-
 * sRGB textures (emoji atlas, background image). The single-channel glyph
 * coverage atlas (`R8Unorm`) is already interpreted as linear alpha by the
 * shader and must NOT be changed.
 *
 * Do not call powf or apply any gamma math outside the helpers below — if
 * you ever catch yourself doing `c.r *= factor` in ui.c that should be
 * perceptual, pass the unmodified Color through the boundary and let this
 * pipeline do the right thing.
 * ========================================================================= */

typedef struct {
    f32 r, g, b, a;
} Color;

#define COLOR_RGB(r8, g8, b8) ((Color){ (r8)/255.0f, (g8)/255.0f, (b8)/255.0f, 1.0f })
#define COLOR_RGBA(r8, g8, b8, a8) ((Color){ (r8)/255.0f, (g8)/255.0f, (b8)/255.0f, (a8)/255.0f })

extern Color g_ansi_colors[256];
void renderer_init_colors(void);

/* sRGB → linear on a single channel (IEC 61966-2-1 piecewise curve). */
static inline f32 srgb_decode(f32 c) {
    if (c <= 0.0f) return 0.0f;
    if (c >= 1.0f) return 1.0f;
    return (c <= 0.04045f) ? (c * (1.0f / 12.92f))
                           : powf((c + 0.055f) * (1.0f / 1.055f), 2.4f);
}

/* Convert an entire sRGB Color to linear. Alpha is linear in both spaces. */
static inline Color color_linear_from_srgb(Color s) {
    return (Color){ srgb_decode(s.r), srgb_decode(s.g), srgb_decode(s.b), s.a };
}

/* Fast inline resolve of the u32 color encoding used by Cell fg/bg.
 * Values 0-255 are ANSI palette indices (direct LUT lookup); bit 24 set
 * (0x01RRGGBB) indicates 24-bit truecolor.  `fallback` is the palette index
 * used when val >= 256 but not truecolor (should be FG_DEFAULT or BG_DEFAULT).
 *
 * Returns an **sRGB** Color — call color_linear_from_srgb when crossing the
 * CPU → GPU boundary. */
static inline Color color_resolve(u32 val, u32 fallback) {
    if (val & 0x01000000) {
        const f32 inv = 1.0f / 255.0f;
        return (Color){
            (f32)((val >> 16) & 0xFF) * inv,
            (f32)((val >>  8) & 0xFF) * inv,
            (f32)( val        & 0xFF) * inv,
            1.0f
        };
    }
    return g_ansi_colors[val < 256 ? val : fallback];
}

/* =========================================================================
 * Font atlas
 * ========================================================================= */

typedef struct {
    u32 texture_id;
    i32 atlas_width;
    i32 atlas_height;
    f32 cell_width;
    f32 cell_height;
    f32 ascent;
    f32 descent;
    f32 line_height;
    struct {
        f32 u0, v0, u1, v1;
        f32 x_offset, y_offset;
        f32 advance;
    } glyphs[256];
    bool loaded;
    void *metal_texture;   /* MTLTexture* when using Metal, NULL otherwise */
    u8   *atlas_bitmap;    /* retained CPU copy for deferred Metal upload */

    /* Color emoji atlas (RGBA). Created lazily on first emoji to keep idle
     * RAM small for users who never type emoji. `metal_device` caches the
     * MTLDevice pointer so the deferred upload path can build the texture
     * without plumbing the device through every call site. */
    void *color_texture;   /* MTLTexture* BGRA8, NULL until first emoji */
    u8   *color_bitmap;    /* RGBA CPU bitmap */
    i32   color_atlas_w;
    i32   color_atlas_h;
    i32   color_atlas_x;   /* packing cursor */
    i32   color_atlas_y;
    i32   color_row_h;
    void *metal_device;    /* weak MTLDevice* for lazy color-atlas creation */
} FontAtlas;

bool font_atlas_create(FontAtlas *atlas, const char *font_path, f32 font_size, f32 dpi_scale, f32 font_weight);
void font_atlas_destroy(FontAtlas *atlas);

/* Load additional fallback fonts (user-configured paths). Call after font_atlas_create.
 * Fonts are added to the end of the fallback chain. Duplicate paths are ignored. */
void font_atlas_load_user_fallbacks(const char **paths, i32 count);

/* Set user-configured fallback font paths. These are loaded automatically by
 * font_atlas_create() after the default system fallbacks. Call before the first
 * font_atlas_create(), or before re-creating the atlas on config changes. */
void font_set_user_fallback_config(const char **paths, i32 count);

/* Ligature: check if codepoints form a ligature and rasterize as combined glyph.
 * Returns the ligature width in cells (0 = no ligature).  Uses a cache keyed
 * by codepoint sequence so CoreText shaping only runs once per unique sequence. */
i32 font_check_ligature(FontAtlas *atlas, const u32 *cps, i32 count,
                        f32 *u0, f32 *v0, f32 *u1, f32 *v1);

/* High-level ligature query: look up cached ligature UVs for a codepoint run.
 * Returns ligature width in cells (0 = no ligature, input rendered normally). */
bool font_get_ligature_uv(FontAtlas *font, const u32 *codepoints, i32 count,
                           f32 *u0, f32 *v0, f32 *u1, f32 *v1);

/* Get glyph UV (with on-demand rasterization for Unicode) */
bool font_get_glyph_uv(FontAtlas *atlas, u32 cp, f32 *u0, f32 *v0, f32 *u1, f32 *v1);
bool font_get_composite_glyph_uv(FontAtlas *atlas, u32 base, const u32 *combining,
                                  u8 count, f32 *u0, f32 *v0, f32 *u1, f32 *v1);

/* async glyph rasterization — background worker thread.
 * start must be called after font_atlas_create + (on macOS Metal) after
 * font_atlas_create_metal_texture so the atlas upload path is ready. */
void font_start_async_raster(FontAtlas *atlas);
void font_stop_async_raster(void);
/* Drain completions from worker, upload to atlas, insert into cache.
 * Returns the number of newly installed glyphs. Main thread only. */
u32  font_drain_raster_completions(FontAtlas *atlas);
/* Version counter bumped whenever new glyphs land — row cache invalidation. */
u32  font_raster_completion_version(void);

/* Synchronously rasterize every non-ASCII codepoint in `utf8_text` that
 * isn't already cached. Use this from UI text rendering paths (file
 * browser, palette, settings) where the normal async fallback returns a
 * space placeholder until a future frame — UI frames are event-driven, so
 * the next render can be arbitrarily far away and the placeholder stays
 * visible. Main thread only; ASCII codepoints are skipped (the warm-on-
 * boot path in font_atlas_create already filled them). Cheap if the text
 * is fully cached. */
void font_warm_text_glyphs(FontAtlas *atlas, const char *utf8_text);
/* Length-aware variant for non-NUL-terminated slices (e.g. one line of a
 * larger buffer). Pass len = SIZE_MAX to scan until NUL. */
void font_warm_text_glyphs_n(FontAtlas *atlas, const char *utf8_text, usize len);

#ifdef USE_METAL
/* Create Metal texture for font atlas (call after renderer_set_gpu) */
void font_atlas_create_metal_texture(FontAtlas *atlas, void *device);
/* Lazily allocate the emoji RGBA atlas texture on first emoji use. */
bool font_ensure_color_atlas_metal(FontAtlas *atlas);
#endif

/* =========================================================================
 * Per-glyph instance data (Pass 2 — GPU instanced)
 * ========================================================================= */

typedef struct {
    f32 x, y;               /* pixel position */
    f32 u0, v0, u1, v1;     /* atlas UV */
    f32 r, g, b;             /* foreground color (ignored for color glyphs) */
    f32 is_color;            /* 0.0 = alpha glyph, 1.0 = color emoji */
    f32 w_cells;             /* width in cells: 1.0 normal, 2.0+ for ligatures */
    f32 a;                   /* per-instance alpha multiplier (1.0 = opaque) */
} GlyphInstance;

/* Per-rect instance. Replaces the old 6-vertex layout — one
 * unit quad is drawn via glDrawArraysInstanced / drawPrimitives:instanceCount:
 * and each instance provides its pixel position, size, and color. */
typedef struct {
    f32 x, y;    /* top-left in framebuffer pixels */
    f32 w, h;    /* size in framebuffer pixels */
    f32 r, g, b, a;
} RectInstance;

/* Per-rounded-rect instance. SDF-based shader produces smooth corners and
 * a real soft drop shadow in a single fragment pass — replaces the legacy
 * 3-step CPU staircase (`ui.c::draw_rounded_top_rect`) and the fake "two
 * stacked alpha rects" shadow trick used for panels/tabs.
 *
 * Shadow color is fixed to black; only `shadow_alpha` is configurable.
 * Per-corner radii allow tab-style top-only rounding (set bottom radii to 0).
 * Color channels stored linear (CPU→GPU boundary already converted). */
typedef struct {
    f32 x, y;                         /* visual rect top-left (NOT shadow extent) */
    f32 w, h;                         /* visual rect size */
    f32 r, g, b, a;                   /* fill color (linear) */
    f32 r_tl, r_tr, r_br, r_bl;       /* per-corner radius in pixels */
    f32 shadow_size;                  /* shadow blur radius (0 = no shadow) */
    f32 shadow_alpha;                 /* shadow opacity (color is black) */
    f32 shadow_offset_y;              /* shadow Y offset (positive = down) */
    f32 shadow_offset_x;              /* shadow X offset (positive = right) */
    f32 border_r, border_g, border_b, border_a;  /* border color (linear), used when border_width > 0 */
    f32 border_width;                 /* border thickness in pixels (0 = no border) */
    f32 inner_shadow_size;            /* inner shadow inset depth (0 = none) */
    f32 inner_shadow_alpha;           /* inner shadow opacity (color is black) */
    f32 _pad0;                        /* pad to 96 bytes for vec4 alignment */
} RRectInstance;

/* Per-line instance — a thick anti-aliased segment. The vertex shader expands
 * a unit quad along the segment with a perpendicular offset; the fragment does
 * 1px edge AA from the perpendicular distance. Additive pipeline (graph-view
 * edges); nothing else depends on it. Color channels stored linear. */
typedef struct {
    f32 x0, y0, x1, y1;   /* endpoints in framebuffer pixels */
    f32 thickness;        /* line width in pixels */
    f32 r, g, b, a;       /* color (linear) */
} LineInstance;

/* =========================================================================
 * Renderer
 * ========================================================================= */

typedef struct {
    void *backend;             /* GPU backend (RendererMetal* when USE_METAL) */

    /* Glyph instanced shader (Pass 2) */
    u32 glyph_shader;
    u32 glyph_vao;
    u32 glyph_quad_vbo;       /* unit quad geometry */
    u32 glyph_instance_vbo;   /* per-glyph instance data */
    i32 glyph_u_projection;
    i32 glyph_u_atlas;
    i32 glyph_u_cell_size;
    /* CPU-side mirror of the glyph shader's u_cell_size uniform. Updated at
     * every glUniform2f(glyph_u_cell_size,...) site in renderer.c so the
     * image path can restore the prior value without a glGetUniformfv GPU
     * readback stall. Zero-initialized by memset in renderer_init, matching
     * the uniform's GL default of (0,0). */
    f32 gl_glyph_cell_size[2];

    GlyphInstance *glyph_instances;
    u32 glyph_count;
    u32 glyph_cap;

    /* Rect batch shader (Pass 1 + 3) — instanced unit quad. */
    u32 rect_shader;
    u32 rect_vao;
    u32 rect_quad_vbo;         /* shared unit quad geometry */
    u32 rect_instance_vbo;     /* per-rect instance data (RectInstance) */
    i32 rect_u_projection;

    RectInstance *rect_batch;  /* stored as RectInstance[] */
    u32  rect_batch_count;
    u32  rect_batch_cap;

    /* Rounded-rect batch — flushed AFTER flat rects, BEFORE glyphs, so
     * rounded panels/tabs/cards layer above plain backgrounds and beneath
     * text. OpenGL backend leaves these unused (graceful flat-rect fallback). */
    RRectInstance *rrect_batch;
    u32  rrect_batch_count;
    u32  rrect_batch_cap;
    u32  rrect_shader;
    u32  rrect_vao;
    u32  rrect_quad_vbo;
    u32  rrect_instance_vbo;
    i32  rrect_u_projection;
    u32  gl_rrect_uniforms_version;

    /* Line batch — thick AA segments (graph edges). Additive pipeline,
     * flushed on demand by the caller. */
    LineInstance *line_batch;
    u32  line_batch_count;
    u32  line_batch_cap;
    u32  line_shader;
    u32  line_vao;
    u32  line_quad_vbo;
    u32  line_instance_vbo;
    i32  line_u_projection;

    /* State */
    i32 screen_width;
    i32 screen_height;
    f32 dpi_scale;
    f32 projection[16];
    f32 clear_color[4];
    f32 ui_cell_w, ui_cell_h;  /* override cell size for UI chrome */

    /* Scissor rect (push/pop — top-left origin, framebuffer pixels) */
    bool scissor_active;
    f32  scissor_x, scissor_y, scissor_w, scissor_h;

    /* Uniform cache — avoid redundant GPU state uploads.
     * Incremented whenever projection or cell size changes; backends compare
     * against their own "last uploaded" version before re-uploading. */
    u32 uniforms_version;
    i32 cached_proj_w, cached_proj_h;   /* last size ortho() was computed for */
    f32 cached_cell_w, cached_cell_h;   /* last cell size baked into uniforms */
    /* OpenGL backend tracks last uploaded version per shader program */
    u32 gl_rect_uniforms_version;
    u32 gl_glyph_uniforms_version;

    FontAtlas font;

    /* Background image state */
    void *bg_texture;       /* MTLTexture* (Metal) or GLuint (OpenGL) — NULL if none */
    i32   bg_image_w;       /* original image width */
    i32   bg_image_h;       /* original image height */
    f32   bg_opacity;       /* opacity for background image (0.0-1.0) */
    i32   bg_mode;          /* 0=stretch, 1=center, 2=tile, 3=fill */
} Renderer;

bool renderer_init(Renderer *r, f32 dpi_scale);
void renderer_destroy(Renderer *r);
void renderer_begin_frame(Renderer *r, i32 width, i32 height);
void renderer_end_frame(Renderer *r);
/* Release renderer-owned idle caches whose rebuild cost is small compared to
 * their resident memory. Does not change visible output; call from the main
 * loop with a monotonic timestamp. */
void renderer_trim_idle_resources(Renderer *r, f64 now_sec);

/* Pass 1 + 3: Rect batching */
void renderer_draw_rect(Renderer *r, f32 x, f32 y, f32 w, f32 h, Color color);
void renderer_flush_rects(Renderer *r);

/* Rounded rect with per-corner radii and optional soft drop shadow.
 * Single GPU fragment pass — SDF computes both fill and shadow.
 * `shadow_size = 0` disables the shadow. Shadow offsets are in pixels:
 * positive Y = down, positive X = right. Metal backend implements the
 * full SDF; OpenGL falls back to a flat rect. */
void renderer_draw_rrect(Renderer *r,
    f32 x, f32 y, f32 w, f32 h,
    Color fill,
    f32 r_tl, f32 r_tr, f32 r_br, f32 r_bl,
    f32 shadow_size, f32 shadow_alpha,
    f32 shadow_offset_x, f32 shadow_offset_y);
void renderer_flush_rrects(Renderer *r);

/* Thick anti-aliased line segment (graph-view edges). Additive batched
 * pipeline on both backends; flush to submit. Color is sRGB (converted to
 * linear at the boundary, like rects). */
void renderer_draw_line(Renderer *r, f32 x0, f32 y0, f32 x1, f32 y1,
                        f32 thickness, Color color);
void renderer_flush_lines(Renderer *r);

/* Bordered variant — replaces the legacy "draw outer rrect for border, then
 * inner rrect for fill" pattern with a single SDF instance. The shader
 * paints `border_color` in the outermost `border_width` pixels and `fill`
 * everywhere inside that ring; AA at both edges. `border_width = 0` is
 * equivalent to renderer_draw_rrect (no border). */
void renderer_draw_rrect_bordered(Renderer *r,
    f32 x, f32 y, f32 w, f32 h,
    Color fill, Color border, f32 border_width,
    f32 r_tl, f32 r_tr, f32 r_br, f32 r_bl,
    f32 shadow_size, f32 shadow_alpha,
    f32 shadow_offset_x, f32 shadow_offset_y);

/* Pressed/inset look — inner shadow darkening from the rect's inside edge.
 * `inner_size` is the inset depth in pixels, `inner_alpha` the opacity at
 * the edge. Useful for pressed buttons, recessed inputs, and focus glow.
 * Combine freely with border + outer shadow on the same instance. */
void renderer_draw_rrect_inset(Renderer *r,
    f32 x, f32 y, f32 w, f32 h, Color fill, f32 radius,
    f32 inner_size, f32 inner_alpha);

/* Backdrop-blur capture: snapshot the in-flight drawable into the renderer's
 * blur source texture. Call this once, AFTER scene rendering and BEFORE
 * drawing any modal panel that wants a blurred background. Implementation
 * ends the current encoder, blits the drawable, then restarts the encoder.
 * Metal-only — OpenGL backend is a no-op (blur falls back to plain rrect). */
void renderer_blur_capture(Renderer *r);

/* Draw a rounded rect whose interior samples the captured blur source with
 * a 13-tap separable Gaussian kernel, then blends `tint` over the blurred
 * sample. Two passes (horizontal + vertical) are issued internally with the
 * same instance buffer; pass `dir_xy` only once via this convenience API.
 * If renderer_blur_capture wasn't called this frame, falls back to a flat
 * rrect with the tint color. */
void renderer_draw_blur_panel(Renderer *r,
    f32 x, f32 y, f32 w, f32 h,
    Color tint,
    f32 r_tl, f32 r_tr, f32 r_br, f32 r_bl);

/* Convenience: uniform corner radius, no shadow. */
static inline void renderer_draw_rrect_simple(Renderer *r,
    f32 x, f32 y, f32 w, f32 h, Color fill, f32 radius) {
    renderer_draw_rrect(r, x, y, w, h, fill, radius, radius, radius, radius,
                        0.0f, 0.0f, 0.0f, 0.0f);
}

/* Convenience: top-only rounded (tab style), no shadow. */
static inline void renderer_draw_rrect_top(Renderer *r,
    f32 x, f32 y, f32 w, f32 h, Color fill, f32 radius) {
    renderer_draw_rrect(r, x, y, w, h, fill, radius, radius, 0.0f, 0.0f,
                        0.0f, 0.0f, 0.0f, 0.0f);
}

/* Pass 2: Queue a glyph instance */
void renderer_push_glyph(Renderer *r, f32 x, f32 y, u32 codepoint, Color fg);
void renderer_flush_glyphs(Renderer *r);

/* bulk append pre-built instances (flushes mid-append if cap hit).
 * Used by the per-row render cache so non-dirty rows become a single memcpy.
 * Color channels in these instances must already be linear RGB. */
void renderer_append_rects(Renderer *r, const RectInstance *src, u32 count);
void renderer_append_glyphs(Renderer *r, const GlyphInstance *src, u32 count);

/* UI text helper (uses glyph instances) */
void renderer_draw_glyph(Renderer *r, u32 codepoint, f32 x, f32 y, Color fg, Color bg);

/* Override glyph cell size (for UI chrome at fixed scale) */
void renderer_set_ui_scale(Renderer *r, f32 cw, f32 ch);
void renderer_reset_ui_scale(Renderer *r);

/* Scissor clipping — clips all subsequent draws to the rect (top-left origin).
 * pop restores full-screen drawing. Both flush pending batches first. */
void renderer_push_scissor(Renderer *r, f32 x, f32 y, f32 w, f32 h);
void renderer_pop_scissor(Renderer *r);

/* Compat aliases */
void renderer_flush_text(Renderer *r);
void renderer_flush_cells(Renderer *r);
void renderer_push_cell(Renderer *r, f32 x, f32 y, u32 codepoint, Color fg, Color bg);

/* Backend init (call after renderer_init) */
#ifdef USE_METAL
void renderer_set_gpu(Renderer *r, void *device, void *layer, void *queue);
#else
void renderer_set_gpu(Renderer *r, void *device, void *layer, void *queue);
#endif
void renderer_set_clear_color(Renderer *r, f32 red, f32 green, f32 blue, f32 alpha);

/* Background image */
bool renderer_load_background_image(Renderer *r, const char *path, void *gpu_device);
void renderer_destroy_background_image(Renderer *r);
void renderer_draw_background_image(Renderer *r);

/* =========================================================================
 * Inline image rendering (Sixel, Liu)
 * ========================================================================= */

/* Render an inline image at pixel position (x, y) with given pixel dimensions.
 * pixels must be RGBA, 4 bytes per pixel. The renderer may cache the texture. */
void renderer_draw_image(Renderer *r, const u8 *pixels, i32 img_w, i32 img_h,
                         f32 x, f32 y, f32 draw_w, f32 draw_h);
void renderer_draw_image_cached(Renderer *r, const void *cache_key, u64 cache_generation,
                                const u8 *pixels, i32 img_w, i32 img_h,
                                f32 x, f32 y, f32 draw_w, f32 draw_h);

#ifdef USE_METAL
/* Metal atlas helpers (called by font.c) */
void renderer_metal_set_atlas(Renderer *r, void *texture);
void renderer_metal_upload_texture(void *texture, i32 x, i32 y, i32 w, i32 h, const void *data);

/* GPU compute: render terminal grid entirely on GPU */
void renderer_compute_terminal(Renderer *r, const void *cells, i32 cols, i32 rows,
                                f32 origin_x, f32 origin_y);
/* Upload ANSI palette to GPU (call once or when theme changes) */
void renderer_upload_palette(Renderer *r);
#endif

#endif /* RENDERER_H */
