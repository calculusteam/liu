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
#include "core/file_stamp.h"
#include <math.h>
#include <stddef.h>   /* offsetof — used by the layout assertions below */

/* Color inputs are stored in sRGB. The GPU path decodes sampled sRGB textures
 * to linear values and encodes output to sRGB; glyph coverage remains linear
 * alpha. Keep color adjustment at these boundary helpers so UI colors are
 * transformed consistently. */

typedef struct {
    f32 r, g, b, a;
} Color;

#define COLOR_RGB(r8, g8, b8) ((Color){ (r8)/255.0f, (g8)/255.0f, (b8)/255.0f, 1.0f })
#define COLOR_RGBA(r8, g8, b8, a8) ((Color){ (r8)/255.0f, (g8)/255.0f, (b8)/255.0f, (a8)/255.0f })

extern Color g_ansi_colors[256];
void renderer_init_colors(void);

/* sRGB → linear on a single channel (IEC 61966-2-1 piecewise curve).
 * This is the EXACT form — it also seeds the LUT below, so it must never
 * be redefined in terms of it. */
static inline f32 srgb_decode(f32 c) {
    if (c <= 0.0f) return 0.0f;
    if (c >= 1.0f) return 1.0f;
    return (c <= 0.04045f) ? (c * (1.0f / 12.92f))
                           : powf((c + 0.055f) * (1.0f / 1.055f), 2.4f);
}

/* 256-entry decode LUT, filled by renderer_init_colors. Every colour that
 * reaches the CPU→GPU boundary originates from 8-bit values (ANSI palette,
 * 0xRRGGBB truecolor, theme hex), so nearest-index lookup is BIT-EXACT for
 * them; sRGB-space modifiers (DIM * 0.797) land between grid points where
 * the re-encoded 8-bit output differs by at most 1 LSB in 0.0002% of inputs
 * — invisible. A table lookup replaces powf on a path that runs for every
 * colour of a full-grid rebuild. */
extern f32  g_srgb_decode_lut[256];
extern bool g_srgb_decode_lut_ready;

static inline f32 srgb_decode_fast(f32 c) {
    /* Negated comparisons so a NaN takes the first branch instead of
     * falling through to index the LUT with (u32)NaN. */
    if (!(c > 0.0f)) return 0.0f;
    if (!(c < 1.0f)) return 1.0f;
    if (g_srgb_decode_lut_ready)
        return g_srgb_decode_lut[(u32)(c * 255.0f + 0.5f)];
    return srgb_decode(c);   /* pre-init callers keep exact behaviour */
}

/* Convert an entire sRGB Color to linear. Alpha is linear in both spaces. */
static inline Color color_linear_from_srgb(Color s) {
    return (Color){ srgb_decode_fast(s.r), srgb_decode_fast(s.g),
                    srgb_decode_fast(s.b), s.a };
}

/* Component-wise blend: t=0 → a, t=1 → b. Used for hover/pressed tinting
 * so a card can lerp its rest fill toward a highlight fill by the eased
 * hover amount. Operates in whatever space the inputs are (call before the
 * sRGB→linear boundary). */
static inline Color color_lerp(Color a, Color b, f32 t) {
    if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
    return (Color){ a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t,
                    a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t };
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
    /* Decoration geometry from the face, in pixels from the top of the cell:
     * the top of the underline and of the strikeout and their thickness, and
     * the stroke width procedurally drawn lines use (the underline's). */
    f32 underline_y, underline_h;
    f32 strike_y, strike_h;
    i32 line_px;
    struct {
        f32 ax0, ay0, ax1, ay1;   /* atlas TEXELS (see FontGlyphRect) */
        f32 x_offset, y_offset;
        f32 advance;
    } glyphs[256];
    bool loaded;
    void *metal_texture;   /* MTLTexture* when using Metal, NULL otherwise */
    void *metal_seed_texture; /* shared immutable prewarmed R8 page (Metal) */
    void *shared_seed;      /* refcounted owner, private to font.c */
    bool alpha_shared;     /* slot 0 aliases the seed until the first private write */
    u8   *atlas_bitmap;    /* retained CPU copy for deferred Metal upload */
    /* The renderer receiving this atlas's texture uploads (RendererMetal on
     * Metal, NULL for OpenGL or headless operation). When set, texels reach
     * the GPU only through renderer_atlas_upload; see the atlas
     * synchronization contract below. */
    void *gpu_uploader;
    /* The newest content version the renderer has prepared for the GPU (or
     * queued ahead of the frame being drawn). A glyph installed later than
     * this is NOT drawable yet: font_get_glyph_rect serves the placeholder for
     * it and marks the row pending, the way the async raster path does. Only
     * consulted while gpu_gated is set (a renderer that prepares uploads at
     * the frame boundary); GL uploads are ordered by the command stream and
     * never gate. */
    u32  gpu_ready_version;
    bool gpu_gated;

    /* Color emoji atlas (RGBA). Created lazily on first emoji to keep idle
     * RAM small for users who never type emoji. `metal_device` caches the
     * MTLDevice pointer so the deferred upload path can build the texture
     * without plumbing the device through every call site. */
    void *color_texture;   /* MTLTexture* BGRA8, NULL until first emoji */
    u32   color_texture_gl;/* GL RGBA8 texture name, 0 until first emoji */
    u8   *color_bitmap;    /* Optional RGBA CPU mirror for color glyph data. It is NULL in standard sessions; writers update it only when allocated. */
    i32   color_atlas_w;
    i32   color_atlas_h;
    i32   color_atlas_x;   /* packing cursor */
    i32   color_atlas_y;
    i32   color_row_h;
    void *metal_device;    /* weak MTLDevice* for lazy color-atlas creation */

    /* What this atlas last reported to the memory ledger (MEM_FONT_CPU /
     * MEM_FONT_GPU). font_atlas_reaccount() recomputes the real totals from
     * the fields above and posts the DIFFERENCE, so no allocation site has to
     * remember to pair an add with a subtract — the alpha atlas doubles up to
     * 4096x4096, the emoji atlas resets and reallocates, and the CPU mirror
     * is freed after the Metal upload but kept forever on GL. A scheme of
     * paired deltas across those paths drifts; recomputing cannot. */
    usize accounted_cpu;
    usize accounted_gpu;

    /* Per-atlas glyph cache, ligature cache, atlas packing cursor and async
     * raster worker (`FontAtlasPriv` — font.c owns the layout, ~250 KB).
     *
     * Per atlas, not process-wide: with one AppState (and therefore one
     * FontAtlas + one GPU atlas texture) per window, a UV recorded by window
     * A's packer names a rectangle in A's texture and nothing at all in B's.
     * A shared cache would make B sample garbage on a "hit", and a shared
     * packing cursor would make B overwrite the pixels A's row caches still
     * point at. The face itself (mmap + stbtt_fontinfo + CoreText objects +
     * fallback chain) stays process-shared and refcounted — see g_face_refs.
     */
    void *priv;
} FontAtlas;

bool font_atlas_create(FontAtlas *atlas, const char *font_path, f32 font_size, f32 dpi_scale, f32 font_weight);
/* Spacing changes the raster cell and baseline, never the face's em size.
 * Apply it before any glyph is packed; cell_width/height are immutable after
 * creation because both the worker and the renderer read them. */
bool font_atlas_create_spaced(FontAtlas *atlas, const char *font_path,
                              f32 font_size, f32 dpi_scale, f32 font_weight,
                              f32 width_scale, f32 height_scale);
/* Build and upload a replacement before releasing the current atlas. On
 * failure the current atlas (including its worker and GPU texture) survives.
 * device is the Metal device, or NULL for GL/headless use. */
bool font_atlas_replace(FontAtlas *atlas, const char *font_path,
                         f32 font_size, f32 dpi_scale, f32 font_weight,
                         f32 width_scale, f32 height_scale, void *device);
void font_atlas_destroy(FontAtlas *atlas);
typedef struct { u32 glyph_capacity, glyph_used, ligature_capacity, ligature_used; usize cpu_bytes; } FontCacheStats;
/* Read on the atlas-owning thread; excludes font faces and pixel buffers. */
void font_atlas_cache_stats(const FontAtlas *atlas,FontCacheStats *stats);
typedef struct { u64 hits, misses; usize bytes; } FontPrewarmStats;
/* Process-wide, bounded prewarm template; the accessor takes the face lock. */
void font_prewarm_stats(FontPrewarmStats *stats);

/* Face-metric options, applied by the next font_atlas_create().
 *   line_height — row-pitch multiplier (Config.line_height, 1.0 = normal)
 *   normalize   — scale each face's em so its cap height matches the reference
 *                 face, and derive the line box from the requested size rather
 *                 than the face's hhea table. Without it, the same point size
 *                 renders 17 % different letter heights and 50 % different row
 *                 pitches across the bundled fonts.
 *   smoothing   — Core Graphics font smoothing (macOS outline dilation). Off
 *                 falls back to the shader-side coverage gamma. */
typedef enum {
    FONT_SMOOTHING_NEVER  = 0,
    /* Full smoothing at every density. */
    FONT_SMOOTHING_ALWAYS = 1,
    /* Light smoothing for an atlas built below 1.5x, none on a Retina
     * display. Undilated, a stem at standard density is one partly lit
     * pixel column and text reads stepped; fully dilated, it reads heavier
     * than the same text at 2x, and small text fills its counters.
     * Resolved per atlas, so a window that changes display follows. */
    FONT_SMOOTHING_AUTO   = 2,
} FontSmoothing;
void font_set_metrics_options(f32 line_height, bool normalize, FontSmoothing smoothing);

/* How an atlas built at `dpi_scale` is smoothed under `mode`. Core Graphics
 * dilates a smoothed outline by the lightness of the colour it is filled
 * with: LIGHT is the dilation it gives dark text, FULL the one it gives
 * light text (what AppKit draws white text on a dark background with). */
enum { FONT_SMOOTH_OFF = 0, FONT_SMOOTH_LIGHT = 1, FONT_SMOOTH_FULL = 2 };
u8 font_smoothing_level(FontSmoothing mode, f32 dpi_scale);

/* Coverage gamma the glyph shader must apply, given the current raster options. */
f32 font_text_gamma(void);

/* Load additional fallback fonts (user-configured paths). Call after font_atlas_create.
 * Fonts are added to the end of the fallback chain. Duplicate paths are ignored. */
void font_atlas_load_user_fallbacks(const char **paths, i32 count);

/* Set user-configured fallback font paths. These are loaded automatically by
 * font_atlas_create() after the default system fallbacks. Call before the first
 * font_atlas_create(), or before re-creating the atlas on config changes. */
void font_set_user_fallback_config(const char **paths, i32 count);

/* Where a glyph lives, in atlas TEXELS, and in which atlas.
 *
 * In texels the same glyph keeps the same rectangle across an atlas GROW,
 * which copies the pixels to the same place in a bigger texture; the shader
 * divides by the size of the texture it is handed. Normalised coordinates (u =
 * x / atlas_width) stored in the row caches would make every cached quad on
 * screen stale on each grow. What still moves glyphs is a shelf reclaim or a
 * colour-atlas reset, and those bump the placement generation
 * (font_atlas_placement_generation) — the only version a cache of these
 * rectangles has to watch.
 *
 * `kind` says which atlas: the two atlases have different sizes and the shader
 * has to know which one to normalise against, and a sign bit is not a place to
 * keep that. */
typedef enum {
    FONT_GLYPH_ALPHA = 0,   /* R8 coverage atlas */
    FONT_GLYPH_COLOR = 1,   /* RGBA colour (emoji) atlas */
    FONT_GLYPH_SEED = 3,    /* immutable shared prewarmed R8 page */
} FontGlyphKind;

typedef struct {
    f32 x0, y0, x1, y1;     /* texels; [x0,x1) x [y0,y1) */
    u8  kind;               /* FontGlyphKind */
} FontGlyphRect;

/* Ligature: check if codepoints form a ligature and rasterize as combined glyph.
 * Returns the ligature width in cells (0 = no ligature).  Uses a cache keyed
 * by codepoint sequence so CoreText shaping only runs once per unique sequence. */
i32 font_check_ligature(FontAtlas *atlas, const u32 *cps, i32 count,
                        FontGlyphRect *out);

/* The cells a row builder takes ligature candidates from: one maximal run of
 * printable ASCII with the same colours and attributes. Filled by the caller
 * with state FONT_LIG_RUN_UNSHAPED; font_check_ligature_run shapes it once, on
 * the first candidate that misses the cache, and skips the candidates that
 * shaping left alone (font.c says why). A run longer than FONT_LIG_RUN_MAX is
 * given as FONT_LIG_RUN_PER_CANDIDATE: every candidate is asked on its own. */
#define FONT_LIG_RUN_MAX 512
enum { FONT_LIG_RUN_UNSHAPED = 0, FONT_LIG_RUN_SHAPED, FONT_LIG_RUN_PER_CANDIDATE };
typedef struct {
    u32 cps[FONT_LIG_RUN_MAX];
    u8  changed[FONT_LIG_RUN_MAX];   /* 1 = the run's own shaping drew another glyph here */
    i32 len;
    i32 state;
} FontLigatureRun;

/* font_check_ligature for the candidate run->cps[at .. at+count). */
i32 font_check_ligature_run(FontAtlas *atlas, FontLigatureRun *run, i32 at, i32 count,
                            FontGlyphRect *out);
/* Total ligature-shaping attempts since startup. */
u64 font_ligature_shapings(void);


/* Get a glyph's atlas rectangle (with on-demand rasterization for Unicode).
 * Returns false when the caller got the SPACE placeholder for a glyph that
 * cannot be produced (atlas exhausted, no face coverage); true otherwise —
 * including a placeholder for a glyph still being rasterised or still on its
 * way to the GPU, which font_placeholder_seen() reports. */
/* `font_glyph_is_wide` below is the one predicate both the rasterizer and the
 * quad ask for how many cells a glyph's bitmap spans. There is deliberately no
 * second spelling of the same question (no font_glyph_span()), so the two
 * cannot drift apart. */
bool font_get_glyph_rect(FontAtlas *atlas, u32 cp, FontGlyphRect *out);

/* A bold or italic cell asks font_get_glyph_rect for its codepoint with these
 * bits set: the glyph is drawn from the family's bold / italic face, or
 * thickened / slanted when the family has none, and cached apart from the
 * regular one. Codepoints stay below 2^21 and a cluster handle uses bit 30,
 * so the two bits never collide with either. */
#define FONT_STYLE_BOLD   0x10000000u
#define FONT_STYLE_ITALIC 0x20000000u
#define FONT_STYLE_MASK   (FONT_STYLE_BOLD | FONT_STYLE_ITALIC)
/* An icon drawn across its own cell and the blank cell after it. */
#define FONT_ICON_WIDE    0x08000000u
/* Every flag a glyph key can carry beside its codepoint. */
#define FONT_KEY_FLAGS    (FONT_STYLE_MASK | FONT_ICON_WIDE)

/* A private-use icon (a symbols font's) that may take the blank cell after
 * it: one cell is narrower than the icons are drawn for. The prompt segment
 * shapes U+E0B0-E0D7 always stay in their cell. */
static inline bool font_icon_spills(u32 cp) {
    return ((cp >= 0xE000 && cp <= 0xF8FF) && !(cp >= 0xE0B0 && cp <= 0xE0D7)) ||
           (cp >= 0xF0000 && cp <= 0x10FFFD);
}

/* The key a cell's glyph is asked for: `cp` with the style bits of a bold or
 * italic cell. Spaces, colour emoji, private-use icons and the shapes drawn
 * from the cell geometry look the same in every style and keep the plain
 * codepoint, so they share one atlas entry. */
u32 font_style_key(u32 cp, bool bold, bool italic);

/* The face that draws `style` (FONT_STYLE_* bits) in this atlas: its
 * PostScript name, and whether bold or italic is synthesised on top of it.
 * False where no Core Text face is loaded. */
bool font_style_face(FontAtlas *atlas, u32 style, char *name, usize n, bool *synthesised);

/* The options the next font_atlas_create builds the face with, as AppConfig
 * spells them: OpenType features ("zero, ss01, -calt"), variation axes
 * ("wght=450"), and ranges drawn from a named face first
 * ("U+E000-U+F8FF=Symbols Nerd Font"). Part of the face's identity, so a
 * change reloads it. */
void font_set_face_options(const char *features, const char *variations,
                           const char *cp_map);

/* Was any glyph since the last reset served as the SPACE placeholder while the
 * raster worker finishes it? Reset before a row and read after it: that is two
 * calls per row rather than a predicate per cell, and it is what lets a
 * completed rasterisation invalidate the rows it actually repairs instead of
 * every row on screen. */
void font_placeholder_reset(FontAtlas *atlas);
bool font_placeholder_seen(const FontAtlas *atlas);

/* True for non-emoji glyphs the grid gives two columns to (CJK, fullwidth).
 * Their atlas raster is two cells wide, so the quad must be too. */
bool font_glyph_is_wide(u32 cp);

/* Can this backend supply a COLOUR glyph for `cp`?
 *
 * Not the same question as utf8_is_emoji(): that asks what Unicode says, this
 * asks what the machine actually has. Core Text falls back across every emoji
 * font the system ships, so macOS answers yes for anything it calls an emoji.
 * The Linux path reads one CBDT font, whose repertoire is whatever that file
 * covers — Noto Color Emoji is missing a few hundred of the codepoints in the
 * emoji blocks. Callers that need to tell "the atlas refused it" apart from
 * "there is no such picture" have to ask this first. */
bool font_color_glyph_available(u32 cp);

/* Same, but for a glyph that will be DRAWN at `cell_w` pixels of advance.
 *
 * Above ~1.1x the atlas cell this rasterises a dedicated variant at that size
 * and caches it, instead of letting the sampler magnify the terminal-sized
 * cell — which is what makes large UI text (markdown headings) look soft.
 * Falls back to font_get_glyph_rect for anything near or below 1:1, for emoji,
 * and whenever the atlas has no room left. */
bool font_get_glyph_rect_sized(FontAtlas *atlas, u32 cp, i32 cell_w,
                               FontGlyphRect *out);

/* async glyph rasterization — background worker thread.
 * start must be called after font_atlas_create + (on macOS Metal) after
 * font_atlas_create_metal_texture so the atlas upload path is ready. */
void font_start_async_raster(FontAtlas *atlas);
/* Stop + join THIS atlas's worker. Each atlas runs its own so that closing one
 * window cannot tear down the rasteriser feeding the others. */
void font_stop_async_raster(FontAtlas *atlas);
/* Drain completions from worker, upload to atlas, insert into cache.
 * Returns the number of newly installed glyphs. Main thread only. */
u32  font_drain_raster_completions(FontAtlas *atlas);
/* True while a glyph is queued for (or rasterised but not yet installed by)
 * the async worker — i.e. while some cell on screen is still being drawn as a
 * placeholder space. The main loop keeps producing frames until it clears. */
bool font_raster_work_pending(const FontAtlas *atlas);
/* The frame the loop OWES the raster worker: true only while a completion is
 * waiting to be installed. Queued-but-unfinished work is not a reason to paint
 * — the frame would change nothing — provided the worker can wake the loop
 * when it finishes (font_set_raster_wake). With no wake registered nothing else
 * would ever bring that frame, so this answers like font_raster_work_pending().
 * Main thread only. */
bool font_raster_frame_owed(const FontAtlas *atlas);
/* Called from the raster worker thread after each completion is published.
 * Must be cheap and thread-safe (the app passes platform_wake_render_loop /
 * platform_wake_event_loop). NULL unregisters. Process-wide, like the face. */
void font_set_raster_wake(void (*wake)(void));
/* Version counter bumped whenever new glyphs land — row cache invalidation. */
u32  font_raster_completion_version(const FontAtlas *atlas);
/* =========================================================================
 * Atlas synchronisation contract — what changed, and what it invalidates
 * =========================================================================
 *
 * Changes to the atlas are told apart because each invalidates a different
 * thing:
 *
 *   content version      a texel was written (a glyph packed, a grow's copy,
 *                        a reclaim's zeroing). Invalidates nothing a CPU cache
 *                        holds; it is what the GPU has to be handed before the
 *                        next draw. The Metal backend stamps every queued
 *                        upload with it and publishes gpu_ready_version once
 *                        the copies for a frame are encoded.
 *   extent version       the texture's size changed (a grow). Texel rects stay
 *                        valid; only the texture OBJECT and its size changed,
 *                        which the renderer's per-frame binding absorbs.
 *   placement generation glyphs MOVED — a shelf reclaim or a colour-atlas
 *                        reset re-issues rectangles that other glyphs held.
 *                        Every cached FontGlyphRect is stale; row caches
 *                        rebuild. Also bumped when the atlas is REPLACED
 *                        (font/DPI change): a new identity is a new placement
 *                        of everything.
 *
 * The CPU-side atlas (what font.c has packed), the GPU-visible atlas (what has
 * been copied into the texture the frame samples) and the frame's snapshot
 * (the generation bound for THIS draw) are three different things. A frame
 * draws with one fixed generation; content that landed after that frame's
 * uploads were prepared is served as the placeholder and repaired by the next
 * frame, exactly like a glyph the async worker has not finished. */
u32  font_atlas_content_version(const FontAtlas *atlas);
u32  font_atlas_extent_version(const FontAtlas *atlas);
u32  font_atlas_placement_generation(const FontAtlas *atlas);
/* Another name for the placement generation. It does not move on a grow. */
u32  font_atlas_layout_version(const FontAtlas *atlas);
/* The renderer reports what it has prepared for the GPU (Metal: at the frame
 * boundary, after the blits for the frame are encoded; GL: font.c's own
 * uploads are ordered by the command stream and mark themselves ready). */
void font_atlas_set_gpu_ready(FontAtlas *atlas, u32 content_version);
u32  font_atlas_gpu_ready_version(const FontAtlas *atlas);
/* Content packed but not yet prepared for the GPU — a frame is owed. */
bool font_atlas_upload_pending(const FontAtlas *atlas);
/* Glyphs packed into an alpha atlas since process start — monotonic, never
 * reset, so a reader keeps its own snapshot. The delta across a workload is how
 * much rasterising it cost, which is the only way to see glyph-cache eviction
 * churn from outside. */
u64  font_glyphs_packed(void);
/* Install the BLOCKING fallback font.c runs when it cannot otherwise prove the
 * GPU has stopped reading the atlas cells its shelf reclaim is about to hand
 * back — see renderer_gpu_drain(). Without it the reclaim rewrites cells that
 * frames still in flight are sampling, which the Metal upload path explicitly
 * says must never happen. NULL (the default) is correct for anything with no
 * GPU behind the atlas.
 *
 * The callback RETURNS whether it synchronised. false keeps the reclaim pending
 * and the space in use; a failed drain must never be treated as a completed
 * one. */
void font_atlas_set_gpu_sync(FontAtlas *atlas, bool (*fn)(void *), void *ctx);

/* Install the predicate used by shelf reclaim to determine whether every submitted
 * frame has finished. It shares the context installed by font_atlas_set_gpu_sync,
 * which must be called first. If no GPU reader is attached, NULL allows reclaim to
 * proceed inline. */
void font_atlas_set_gpu_idle(FontAtlas *atlas, bool (*fn)(void *));

/* What font_atlas_service_reclaim() wants the caller to do this turn.
 *
 * The reclaim overwrites atlas cells that earlier frames drew from, so it may only
 * run when NOTHING on the GPU is reading them. "Nothing" has to be true at the
 * instant of the reclaim, not at the instant of some earlier snapshot — a mark
 * taken once and re-asked later says nothing about the frames submitted in
 * between, and those frames sample the same atlas.
 *
 * So the caller is part of the mechanism: while the answer is QUIESCE it must not
 * produce a frame. That is what makes the queue drain, and it costs at most the
 * in-flight depth in dropped frames (~3 vblanks) on an event that already forces
 * every row cache to rebuild. Nothing blocks: the UI thread keeps reading the
 * keyboard and draining every PTY throughout. */
typedef enum {
    /* Nothing pending, or the request was abandoned (see below). Paint normally. */
    FONT_RECLAIM_NONE = 0,
    /* Performed. Every published UV into the reclaimed area is now stale — the
     * layout generation has moved and app_atlas_layout_stale() reports it. */
    FONT_RECLAIM_DONE,
    /* Pending, and the GPU is still working. PAINT NORMALLY and ask again next
     * turn: Liu paints nothing when idle, so a burst that ends empties the queue
     * within a couple of frames and nothing is dropped. */
    FONT_RECLAIM_WAIT,
    /* Pending, and waiting for natural idle did not work — something is producing
     * frames continuously. DO NOT PRODUCE A FRAME this turn, and come back. */
    FONT_RECLAIM_QUIESCE,
} FontReclaimState;

/* Turns spent waiting for the GPU to go idle on its own before frames are
 * suppressed. Costs nothing visible (the atlas keeps drawing; only the one
 * glyph that asked for space shows its placeholder) and covers the common case,
 * because an event-driven terminal stops submitting as soon as the output burst
 * ends. */
#define FONT_RECLAIM_IDLE_TURNS 8

/* How long the caller may be asked to withhold frames before the reclaim is
 * ABANDONED. A wall clock, deliberately, because an attempt count is not a
 * measure of time — 120 deferred attempts is two seconds only if something is
 * asking for frames at 60 Hz, and a deferred frame counts as an attempt too.
 * Past this the request is dropped and painting resumes: the codepoint that
 * asked for space keeps its placeholder until the next request, which is a
 * degraded picture rather than a frozen or blocked one. */
#define FONT_RECLAIM_QUIESCE_SEC 0.75
/* Callback invoked before an atlas resize. Flush queued glyph instances so
 * their UV coordinates are drawn against the atlas dimensions used to create
 * them. It uses the context shared with gpu_sync. */
void font_atlas_set_gpu_flush(FontAtlas *atlas, void (*fn)(void *));

/* True while a reclaim has been ASKED FOR and not yet performed.
 *
 * This has to reach the frame gates, and the reason is a circle that is easy to
 * close the wrong way round: the deferred reclaim does not move the layout
 * generation — it cannot, nothing has been relaid out yet — and it answers its
 * codepoint with the exhausted sentinel rather than queueing a raster. So
 * font_raster_work_pending() is false, app_atlas_layout_stale() is false, and
 * the only thing that performs the reclaim runs INSIDE a frame. With the caret
 * off and nothing else animating, that frame need never come and those cells
 * would stay blank until some unrelated redraw happens. */
bool font_atlas_reclaim_pending(const FontAtlas *atlas);

/* Perform a shelf reclaim the rasteriser asked for but could not do safely.
 *
 * MUST be called where no command buffer is open — the top of a frame, before
 * anything begins encoding. That is half the barrier; renderer_gpu_idle() is the
 * other half, and the caller MUST honour a FONT_RECLAIM_QUIESCE answer by not
 * producing a frame, or the reclaim can never become safe.
 *
 * How many reclaims were abandoned at the quiesce bound is available through
 * font_atlas_reclaim_abandons() — a non-zero count means some codepoints kept
 * their placeholder longer than they should have. */
/* `now` is the caller's monotonic clock. Passed in rather than read here because
 * font.c deliberately does not depend on platform/platform.h (headless targets
 * compile it without the platform layer), and because a bound the caller owns can
 * be exercised without sleeping through it. */
FontReclaimState font_atlas_service_reclaim(FontAtlas *atlas, f64 now);
u32  font_atlas_reclaim_abandons(const FontAtlas *atlas);


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

/* 36 bytes. A colour channel whose source is an 8-bit palette entry or an
 * 8-bit hex triple is stored in a byte, and so are two booleans-in-disguise,
 * rather than spending an f32 on each.
 *
 * The reason to care is not the struct, it is the per-frame COPY. Every
 * instance is memcpy'd out of the row cache into the frame's ring buffer and
 * read back by the vertex fetch, once per painted frame — a 4K grid of glyphs
 * is ~1.3 MB of that, and instance-buffer size weighs on both performance and
 * power draw.
 *
 * COLOURS ARE sRGB HERE, not linear, and that is a deliberate inversion of the
 * banner at the top of this file. Eight bits of LINEAR colour bands visibly in
 * the dark end — avoiding exactly that is what sRGB encoding is for — so the
 * decode moves from the CPU to the vertex shader, which is the one place it
 * can be done per-glyph rather than per-frame-per-cell. The values stored are
 * therefore bit-exact for every colour that came from a palette entry or a hex
 * triple, and sRGB-space modifiers (DIM * 0.797) round to the nearest 1/255,
 * the same tolerance renderer.h's own decode LUT already accepts. Rects and
 * rrects are unchanged: they still cross the boundary on the CPU.
 *
 * `bg` tells the shader what a glyph is being drawn ON, which two features
 * need: a minimum-contrast floor for unreadable theme/content combinations,
 * and the luminance correction that reconciles gamma-correct linear blending
 * with the weight users expect — correct blending lands BRIGHTER than the
 * sRGB-space blend the eye is trained on (0.664 vs 0.500 at half coverage), so
 * it renders light-on-dark heavier and dark-on-light lighter. `bg[3] == 0`
 * means "not known" — the UI-chrome paths do not track a background — and both
 * features fall back to passthrough for those. */
/* Which texture an instance samples, and therefore what its texel rectangle
 * is measured in. The shader normalises the rect by that texture's own size.
 *   ALPHA  the R8 glyph atlas (texture slot 0)
 *   COLOR  the RGBA emoji atlas (slot 1)
 *   IMAGE  a standalone RGBA texture bound at slot 1 for one draw — inline
 *          images, icons, GIF frames. Its rect is in the IMAGE's own pixels. */
enum {
    GLYPH_TEX_ALPHA = 0,
    GLYPH_TEX_COLOR = 1,
    GLYPH_TEX_IMAGE = 2,
    GLYPH_TEX_SEED = 3,
};

typedef struct {
    f32 x, y;               /* top-left in framebuffer pixels */
    f32 ax0, ay0, ax1, ay1; /* texel rect in the texture named by tex_kind */
    u8  fg[4];              /* foreground sRGB rgb + per-instance alpha */
    u8  bg[4];              /* background sRGB rgb + "known" flag in [3] */
    u8  tex_kind;           /* GLYPH_TEX_* */
    u8  w_cells;            /* width in cells: 1 normal, 2+ wide / ligature */
    u8  _pad[2];
} GlyphInstance;

/* sRGB float channel -> the 8-bit value it came from. Round-to-nearest, so a
 * colour that originated as an 8-bit palette entry survives the round trip
 * exactly. */
static inline u8 color_to_u8(f32 c) {
    if (!(c > 0.0f)) return 0;              /* also catches NaN */
    if (c >= 1.0f) return 255;
    return (u8)(c * 255.0f + 0.5f);
}

/* Foreground plus the per-instance alpha multiplier (modal fades, the tab
 * strip's open/close animation). `fg` is sRGB — do NOT pass a linearised
 * colour, the shader decodes. */
static inline void glyph_set_fg(GlyphInstance *gi, Color fg, f32 alpha) {
    gi->fg[0] = color_to_u8(fg.r);
    gi->fg[1] = color_to_u8(fg.g);
    gi->fg[2] = color_to_u8(fg.b);
    gi->fg[3] = color_to_u8(alpha);
}

/* The cell background this glyph sits on, for min-contrast and blend
 * correction. Only the terminal grid knows this; everything else calls
 * glyph_set_no_bg. */
static inline void glyph_set_bg(GlyphInstance *gi, Color bg) {
    gi->bg[0] = color_to_u8(bg.r);
    gi->bg[1] = color_to_u8(bg.g);
    gi->bg[2] = color_to_u8(bg.b);
    gi->bg[3] = 255;
}

static inline void glyph_set_no_bg(GlyphInstance *gi) {
    gi->bg[0] = gi->bg[1] = gi->bg[2] = gi->bg[3] = 0;
}

/* ---- The surface UI text is assumed to sit on --------------------------------
 *
 * The blend correction lives in the shader and needs a background. A glyph that
 * carries none is left out of it and, because a corrected glyph also skips the
 * shader's 1/gamma stem darkening, it is the one that KEEPS that darkening: UI
 * text (the command palette, tabs, menus, the editor) would draw with the raw
 * linear composite plus the gamma boost, visibly heavier than the terminal text
 * beside it, which is corrected down to the familiar weight.
 *
 * UI chrome has no cell behind it, but it does have a surface: the window's own
 * clear colour, which every panel is derived from. So the UI-text emitters hand
 * the shader THAT as the background and the terminal's arithmetic applies
 * unchanged. Doing it here rather than in the shader is deliberate — it needs no
 * new uniform and no metallib rebuild, and it reaches both backends from one
 * definition.
 *
 * The surface is judged against the TEXT, not the other way round: text can only
 * be legible on something it contrasts with, so when it does not contrast with
 * the window surface (white text on an accent button in a light theme, dark text
 * on a light popover in a dark one) it must be sitting on the opposite extreme,
 * and that is what gets assumed. A rule on the text colour alone does not work —
 * dim UI text (an inactive tab, the status bar) sits at 0.39-0.49 sRGB lightness
 * in nine built-in dark themes, on the wrong side of any "light text means dark
 * surface" threshold, and would be thickened instead of thinned.
 *
 * Only the coverage arithmetic changes: min-contrast is off unless
 * LIU_TEXT_CONTRAST is set, and when it is, it also sees these glyphs against
 * the assumed surface. */
#define GLYPH_ASSUMED_BG_MIN_RATIO 1.5f   /* WCAG-style; below it, text ~ its surface */

static inline f32 glyph_lin_lum(Color c) {
    return 0.2126f * srgb_decode(c.r) + 0.7152f * srgb_decode(c.g) +
           0.0722f * srgb_decode(c.b);
}

/* `surface` is the window surface in sRGB, `surface_l` its linear luminance
 * (Renderer::text_surface / text_surface_l). Returns the sRGB background to
 * pass to glyph_set_bg for a UI glyph drawn in `fg`. */
static inline Color glyph_assumed_bg(Color fg, Color surface, f32 surface_l) {
    f32 fl = glyph_lin_lum(fg);
    f32 hi = fl > surface_l ? fl : surface_l;
    f32 lo = fl > surface_l ? surface_l : fl;
    if ((hi + 0.05f) / (lo + 0.05f) >= GLYPH_ASSUMED_BG_MIN_RATIO) return surface;
    /* Same colour as the surface, so it is not on the surface: the opposite
     * extreme, decided by which side the surface is on (0.2 linear ~ 0.48 sRGB). */
    return surface_l > 0.2f ? (Color){0.0f, 0.0f, 0.0f, 1.0f}
                            : (Color){1.0f, 1.0f, 1.0f, 1.0f};
}

/* The GPU reads this struct through a shader-side declaration it cannot check
 * against — `struct GlyphInstance` in LiuShaders.metal and the attribute
 * pointers in renderer.c. A mismatch does not fail to build and does not
 * crash: the vertex stage reads the wrong bytes and text comes out in wrong
 * colours, wrong places or not at all. So pin what can be pinned here, and
 * change all three together. */
_Static_assert(sizeof(GlyphInstance) == 36,
               "GlyphInstance changed size — update LiuShaders.metal's struct "
               "AND renderer.c's glVertexAttribPointer offsets to match");
_Static_assert(offsetof(GlyphInstance, fg) == 24, "GlyphInstance.fg moved");
_Static_assert(offsetof(GlyphInstance, bg) == 28, "GlyphInstance.bg moved");
_Static_assert(offsetof(GlyphInstance, ax0) == 8, "GlyphInstance.ax0 moved");
_Static_assert(offsetof(GlyphInstance, tex_kind) == 32, "GlyphInstance.tex_kind moved");
_Static_assert(offsetof(GlyphInstance, w_cells) == 33, "GlyphInstance.w_cells moved");

/* Per-rect instance. One unit quad is drawn via glDrawArraysInstanced /
 * drawPrimitives:instanceCount: and each instance provides its pixel position,
 * size, and color. */
typedef struct {
    f32 x, y;    /* top-left in framebuffer pixels */
    f32 w, h;    /* size in framebuffer pixels */
    f32 r, g, b, a;
} RectInstance;

/* Backend-owned immutable draw data. UI handles may be dropped without a
 * current GL context; reclamation happens on the owning renderer. */
typedef struct RendererRetainedBatch RendererRetainedBatch;
struct RendererRetainedBatch {
    void (*release)(RendererRetainedBatch *);
    u32 count;
};
static inline void renderer_retained_release(RendererRetainedBatch **batch) {
    if (batch && *batch) { (*batch)->release(*batch); *batch = NULL; }
}
typedef struct {
    bool glyphs;
    union { GlyphInstance *const *glyph_rows; RectInstance *const *rect_rows; } data;
    const i32 *counts;
    i32 rows;
    /* Optional per-row content versions. A version changes whenever that
     * row's packed bytes change; the batch revision still covers any change. */
    const u64 *versions;
} RendererRows;
typedef struct {
    u64 uploads, reuses, upload_bytes, live_bytes;
} RendererRetainedStats;

/* Per-rounded-rect instance. SDF-based shader produces smooth corners and a
 * real soft drop shadow in a single fragment pass.
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

/* Terminal caret in framebuffer pixels. Color is sRGB, like draw_rect. The
 * native presenter publishes it with the frame; it must never get ahead of
 * queued terminal content. A zero-initialized value hides the caret. */
typedef struct {
    f32 x, y, w, h;
    Color color;
    bool visible, blink;
} RendererCursor;

/* Terminal chrome uses its existing framebuffer geometry and painter order.
 * Eight panes can each carry a track, thumb, failure mark and focus wash,
 * with up to seven split dividers. */
#define RENDERER_TERMINAL_RECT_CAP 40
typedef struct {
    f32 x, y, w, h, radius;
    Color color;
} RendererTerminalRect;

typedef struct {
    void *backend;             /* GPU backend (RendererMetal* when USE_METAL) */
    void *retained_store;
    void *retained_draw_buffer; /* borrowed only during a retained batch flush */
    f32 draw_origin_x, draw_origin_y;
    bool cursor_layer_allowed;   /* UI has no overlay above the terminal */
    bool cursor_blink_in_frame;  /* last frame requires a GPU blink repaint */
    bool idle_snapshot_allowed;  /* opaque terminal, no wallpaper/blur/overlay */

    /* Glyph instanced shader (Pass 2) */
    u32 glyph_shader;
    u32 glyph_vao;
    u32 glyph_quad_vbo;       /* unit quad geometry */
    u32 glyph_instance_vbo;   /* per-glyph instance data */
    i32 glyph_u_projection;
    i32 glyph_u_atlas;
    i32 glyph_u_color_atlas;
    /* Two sampler objects for the glyph atlas, mirroring the Metal backend's
     * `sampler` / `samplerNearest` pair. Which one a glyph batch gets is
     * decided per flush; see renderer_flush_glyphs. */
    u32 glyph_sampler_linear;
    u32 glyph_sampler_nearest;
    /* renderer_gpu_idle()'s state on the OpenGL backend: a GLsync placed at the
     * end of every frame (void* so this header stays free of GL types) and the
     * count of frames whose fence has been observed signalled. GL issues commands
     * in order, so "the fence after the last frame has signalled" means every
     * draw of that frame and all before it has finished — the same statement the
     * Metal backend makes from its completion counters, which is where these two
     * are unused. */
    void *gl_sync;
    u64   gl_frames_done;
    i32 glyph_u_cell_size;
    i32 glyph_u_text_gamma;
    i32 glyph_u_min_contrast;
    i32 glyph_u_blend_correct;
    i32 glyph_u_text_weight;
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
    /* Set for the duration of one flush by renderer_draw_rrect_translucent():
     * the batch is submitted with a pipeline that writes colour and LEAVES THE
     * ALPHA CHANNEL ALONE, so a translucent window keeps showing through the
     * shape. Never left armed across draws — see that function. */
    bool rrect_keep_dst_alpha;

    /* Line batch — thick AA segments (graph edges). Additive pipeline,
     * flushed on demand by the caller. */
    LineInstance *line_batch;
    u32  line_batch_count;
    u32  line_batch_cap;
    bool line_batch_stroke; /* joined strokes use pairs of LineInstance records */
    u32 stroke_shader,stroke_vao;
    bool stroke_shader_tried;
    i32 stroke_u_projection;
    u32  line_shader;
    u32  line_vao;
    u32  line_quad_vbo;
    u32  line_instance_vbo;
    i32  line_u_projection;

    /* State */
    i32 screen_width;
    /* No GPU slot/drawable was available. Keep the window dirty and return
     * to event processing instead of treating this attempt as a paint. */
    bool frame_deferred;
    bool frame_waiting_drawable; /* reason for this attempt, not a GPU error */
    bool frame_waiting_prepare;
    /* Process-lifetime, thread-safe callback; never captures a window. */
    void (*frame_ready_wake)(void);
    bool observe_presentation; /* set by the UI before each frame, not a stale global toggle */
    i32 screen_height;
    f32 dpi_scale;
    f32 projection[16];
    f32 clear_color[4];
    f32 ui_cell_w, ui_cell_h;  /* override cell size for UI chrome */
    /* CPU time spent inside the backend's drawable acquisition for the most
     * recent frame. Metal fills it unconditionally; OpenGL has no equivalent
     * acquire and leaves it zero. Main's present grid uses this feedback to
     * move waiting into platform_wait_events instead of repeating it inside
     * app_render where input and PTY readiness cannot interrupt it. */
    f64 last_drawable_wait_sec;

#ifdef LIU_RENDER_REPLAY_PROBE
    /* Per-frame, per-renderer locals for the measurement-only replay probe.
     * No atomics: frame construction is main-thread owned. The Metal GPU tail
     * is the sole cross-thread series and has its own atomic histograms. */
    u64 replay_cache_to_renderer_bytes;
    u64 replay_cache_to_renderer_ns;
    u64 replay_renderer_to_backend_bytes;
    u64 replay_renderer_to_backend_ns;
    u64 replay_glyph_instances;
    u64 replay_rect_instances;
    u64 replay_deco_instances;
    u32 replay_terminal_panes;
    u32 replay_rows_rebuilt;
    bool replay_terminal_upload;
#endif

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

    /* OpenGL inline-image texture cache (`GLImageCache*`, heap-allocated by
     * renderer_init in the OpenGL backend, NULL under Metal — which keeps its
     * own cache on RendererMetal). Per-renderer because GL texture NAMES are
     * per-context: a process-global cache hands window B a name minted in
     * window A's context, which samples as garbage or black. */
    void *gl_image_cache;

    /* OpenGL GPU-time probe, NULL until frame timing is enabled. Under Metal,
     * GPU time is read from command-buffer completion instead. */
    void *gl_gpu_timer;

    /* Background image state */
    void *bg_texture;       /* MTLTexture* (Metal) or GLuint (OpenGL) — NULL if none */
    i32   bg_image_w;       /* original image width */
    i32   bg_image_h;       /* original image height */
    usize bg_gpu_bytes;     /* backend-reported retained bytes (GL private path) */
    f32   bg_opacity;       /* opacity for background image (0.0-1.0) */
    i32   bg_mode;          /* 0=stretch, 1=center, 2=tile, 3=fill */

    /* OpenGL wallpaper pipeline. Unused under Metal (which keeps the
     * equivalent on RendererMetal) and lazily built on the first
     * renderer_load_background_image, mirroring ensure_bg_image_pipeline —
     * most sessions never set a wallpaper and should not pay for the program.
     *
     * Per-renderer, not file-static, for the reason spelled out on
     * gl_image_cache: GL object names are context-local, so a shared program or
     * VAO name minted in window A means nothing in window B's context. */
    u32 bg_shader;
    u32 bg_vao;
    u32 bg_quad_vbo;
    i32 bg_u_projection;
    i32 bg_u_pos_size;      /* vec4: xy = top-left px, zw = size px */
    i32 bg_u_uv;            /* vec4: xy = uv scale, zw = uv offset */
    i32 bg_u_opacity;
    i32 bg_u_tex;

    /* Window opacity, mirrored from config every frame (1.0 = opaque).
     * Read by the background/clear path; glyphs ignore it and always draw at
     * full alpha, which is what keeps text solid at any opacity. */
    f32   win_opacity;

    /* Text rendering knobs the glyph shader reads through its uniforms.
     *
     * Both need the per-glyph background that GlyphInstance carries, so
     * both are inert for UI chrome (which has none) and for colour glyphs.
     * Resolved once in renderer_init from the environment.
     *
     *   LIU_TEXT_CONTRAST=<ratio>  WCAG floor, e.g. 3.0. <= 1.0 (default) off.
     *   LIU_TEXT_BLEND=linear      plain linear blending. Default: corrected.
     *
     * The correction is the default because compositing in linear light
     * draws light text on dark heavier than a gamma-space composite; with
     * Core Text font smoothing off as well (config.c's default) the weight
     * matches the gamma-space composite in both polarities. */
    f32   text_min_contrast;   /* <= 1.0 = off */
    f32   text_blend_correct;  /* 0 or 1 */
    /* The window surface UI text is assumed to sit on — see glyph_assumed_bg.
     * Kept beside the clear colour by renderer_set_clear_color, in sRGB (the
     * form glyph_set_bg takes) plus its linear luminance. Zero until the first
     * clear colour is set, which reads as a black surface: the right guess for
     * the frames before a theme is applied. */
    Color text_surface;
    f32   text_surface_l;
    /* The manual weight (AppConfig.text_weight_*, TEXT_WEIGHT_MIN..MAX; the
     * shader erodes / dilates the glyph mask by it),
     * mirrored here by app_apply_config. Two values because one flush draws
     * ONE class of text: `ui_cell_w > 0` is "chrome or magnified text" and 0 is
     * the terminal grid at the atlas cell — the split the two samplers already
     * follow. The weight is a per-flush uniform, never a per-glyph field, so a
     * change repaints from the row caches with nothing to rebuild. */
    f32   text_weight_terminal;
    f32   text_weight_ui;
} Renderer;

/* The weight the CURRENT glyph batch draws with. Both backends ask this at
 * flush time, so the choice of class is made in exactly one place. */
static inline f32 renderer_text_weight(const Renderer *r) {
    return r->ui_cell_w > 0.0f ? r->text_weight_ui : r->text_weight_terminal;
}

/* Called by both backends' renderer_set_clear_color with the STRAIGHT sRGB
 * channels, before they are premultiplied by the window's opacity. */
static inline void renderer_note_text_surface(Renderer *r, f32 red, f32 green, f32 blue) {
    r->text_surface = (Color){red, green, blue, 1.0f};
    r->text_surface_l = glyph_lin_lum(r->text_surface);
}

/* The background a UI-text glyph in `fg` is emitted with: the assumed surface
 * (see glyph_assumed_bg), or none when blend correction is off — the shader
 * then applies its gamma to the raw mask exactly as it always did. */
static inline void glyph_set_chrome_bg(GlyphInstance *gi, const Renderer *r, Color fg) {
    if (r->text_blend_correct > 0.5f)
        glyph_set_bg(gi, glyph_assumed_bg(fg, r->text_surface, r->text_surface_l));
    else
        glyph_set_no_bg(gi);
}

static inline usize renderer_batch_cpu_bytes(const Renderer *r) {
    if (!r) return 0;
    /* Each capacity is only charged while its buffer is actually held. Neither
     * renderer_destroy zeroes the caps — Metal's NULLs the pointers, the GL one
     * does not even do that — so reading the caps alone reports a renderer that
     * has been torn down as still holding its peak, i.e. a permanent phantom in
     * MEM_RENDER_CPU. The pointer is the thing that says the memory exists. */
    usize n = 0;
    if (r->glyph_instances) n += (usize)r->glyph_cap       * sizeof(GlyphInstance);
    if (r->rect_batch)      n += (usize)r->rect_batch_cap  * sizeof(RectInstance);
    if (r->rrect_batch)     n += (usize)r->rrect_batch_cap * sizeof(RRectInstance);
    if (r->line_batch)      n += (usize)r->line_batch_cap  * sizeof(LineInstance);
    return n;
}

/* Batch capacity plus backend-owned CPU metadata (not GPU resources and not
 * font mirrors, which have their own ledger classes). */
usize renderer_cpu_bytes(const Renderer *r);


/* UI layout reserves a nominal 1:2 text box and passes its top as the draw y.
 * The glyph quad uses the selected font's actual atlas aspect and can be
 * taller than that box. Top-aligning both therefore places text low inside
 * controls. This helper centers chrome glyphs within the allocated box. */
static inline f32 renderer_ui_glyph_dy(const Renderer *r) {
    if (!r || r->ui_cell_w <= 0.0f || r->ui_cell_h <= 0.0f) return 0.0f;
    if (r->font.cell_width <= 0.0f || r->font.cell_height <= 0.0f) return 0.0f;
    /* Same rounding the draw path applies, or the centring offset would be
     * derived from a height the glyph is not actually drawn at. */
    f32 gch = roundf(r->ui_cell_w * (r->font.cell_height / r->font.cell_width));
    return (r->ui_cell_h - gch) * 0.5f;
}

bool renderer_init(Renderer *r, f32 dpi_scale);
void renderer_destroy(Renderer *r);
void renderer_begin_frame(Renderer *r, i32 width, i32 height);
void renderer_end_frame(Renderer *r);
#ifdef USE_METAL
/* Ordered observations in platform_time_sec's clock domain. when==0 denotes
 * an unpresented frame; when<0 reports a telemetry overflow, not a duration. */
bool renderer_take_presentation(Renderer *r, f64 *through, f64 *when);
/* Whether those stamps are "on the layer" (the IOSurface presenter, which has
 * no hardware presentation time; the glass follows within one refresh) rather
 * than a presentation time. The bench panel labels its line accordingly. */
bool renderer_presentation_is_layer_time(Renderer *r);
/* True when the native layer owns this cursor, including an empty/clipped
 * cursor. False asks the caller to use the ordinary rect path (GL-compatible). */
bool renderer_set_terminal_cursor(Renderer *r, RendererCursor cursor);
/* All-or-nothing: false leaves every rect to the ordinary GPU draw path — and
 * false is the answer unless LIU_NATIVE_CHROME=1 was set at launch (the native
 * path is opt-in; see renderer_metal.m). */
bool renderer_set_terminal_rects(Renderer *r, const RendererTerminalRect *rects, u32 count);
/* The opt-in rule itself, as a function of LIU_NATIVE_CHROME's value: only
 * exactly "1" turns the native path on; unset, empty or anything else is off. */
bool renderer_native_chrome_wanted(const char *env);
/* Memory-pressure trim. Preserves shown, GPU-leased and compositor-used images. */
usize renderer_reclaim_presenter(Renderer *r);
#endif
/* Release renderer-owned idle caches whose rebuild cost is small compared to
 * their resident memory. Does not change visible output; call from the main
 * loop with a monotonic timestamp and this renderer's GL context current.
 * Retired handles are collected even without another frame. */
void renderer_trim_idle_resources(Renderer *r, f64 now_sec);

/* Drop every cached image texture that was not drawn in the last frame or so,
 * returning the bytes released. For memory pressure only — the idle sweep
 * above is the ordinary path. A dropped entry re-uploads from the CPU image
 * its owner still holds, so this costs an upload and never a picture.
 * Implemented on both backends: the GL one has its own GLImageCache with the
 * same shape, including the oversize slot that ordinary byte-budget pressure
 * inside the pool can never reclaim. */
usize renderer_reclaim_image_cache(Renderer *r);

/* =========================================================================
 * Blur diagnostic (LIU_BLUR_DEBUG) — names any single draw big and solid
 * enough to switch the window blur off underneath it.
 *
 * It lives HERE, header-inline, rather than in renderer.c: that whole file is
 * wrapped in `#ifndef USE_METAL`, so a probe there would exist only on the
 * OpenGL backend — everywhere except macOS, the one platform that has a
 * window blur at all. It cannot move to a shared .c either, because the
 * headless Metal test harnesses link renderer_metal.m on its own and an extern
 * would leave them with an unresolved symbol. Exactly one TU per build calls
 * it (renderer.c or renderer_metal.m), so the dedupe state below behaves as a
 * single definition would.
 *
 * What it answers: the rect/rrect pipelines blend the alpha channel with
 * MTLBlendOperationMax, so framebuffer alpha never accumulates — a hundred
 * translucent cell fills leave the window exactly as see-through as the clear
 * did. The only way to lose the blur by drawing is a single surface at (or
 * near) alpha 1.0, and then it goes precisely where that surface is. Watching
 * the clear colour, the window flags and the backdrop layer all miss this.
 *
 * Threshold: covers a third of the window and is at least 95% opaque. Reported
 * once per distinct (size, alpha) so a surface that is drawn every frame — a
 * legitimately opaque full-window view — states itself once instead of
 * flooding the log.
 * ========================================================================= */

/* Mirrors platform/platform.h. Redeclared rather than #included so renderer.h
 * does not drag the whole platform interface into every consumer; the
 * prototypes must stay identical to the ones there. */
bool platform_blur_debug_enabled(void);
void platform_blur_debug_note(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

static inline void renderer_blur_probe_draw(Renderer *r, f32 x, f32 y,
                                            f32 w, f32 h, f32 alpha,
                                            const char *kind) {
    if (!platform_blur_debug_enabled()) return;
    if (alpha < 0.95f) return;
    f32 screen = (f32)r->screen_width * (f32)r->screen_height;
    if (screen <= 0.0f) return;
    f32 area = w * h;
    if (area < screen * 0.33f) return;

    static f32 s_last_area = -1.0f;
    static f32 s_last_alpha = -1.0f;
    if (fabsf(area - s_last_area) < 1.0f && fabsf(alpha - s_last_alpha) < 0.01f)
        return;
    s_last_area = area;
    s_last_alpha = alpha;
    platform_blur_debug_note(
        "OPAQUE %s %.0fx%.0f at (%.0f,%.0f) alpha=%.3f covers %.0f%% of the "
        "window — the blur cannot show through this",
        kind, (double)w, (double)h, (double)x, (double)y, (double)alpha,
        (double)(area / screen * 100.0f));
}

/* Texture-upload accounting, Metal only. Every glyph, emoji and image upload
 * goes through renderer_metal_upload_texture, so these three are the whole
 * picture of what the CPU spends handing pixels to the GPU. Monotonic and
 * never reset: each reader keeps its own snapshot, the way the GPU-timing
 * counters do — the first version had each consumer zeroing them and running
 * two printed 0. Zero on the OpenGL build. */
void renderer_metal_upload_stats(u64 *out_uploads, u64 *out_bytes, u64 *out_ns);

/* Pass 1 + 3: Rect batching */
void renderer_draw_rect(Renderer *r, f32 x, f32 y, f32 w, f32 h, Color color);
/* Draw calls and instances submitted since the last reset. Read and zeroed
 * by the frame probe in main.c — the UI calls flush from ~490 sites and how
 * many fire per frame is a measurement, not a code-reading exercise. */
extern u32 g_liu_draw_calls;
extern u32 g_liu_draw_instances;

void renderer_flush_rects(Renderer *r);

/* Rounded rect with per-corner radii and optional soft drop shadow.
 * Single GPU fragment pass — SDF computes both fill and shadow.
 * `shadow_size = 0` disables the shadow. Shadow offsets are in pixels:
 * positive Y = down, positive X = right. Both backends implement the full SDF
 * — fill, per-corner radii, border, inner shadow and drop shadow — in one
 * fragment pass; see rrect_frag_src in renderer.c for the GL twin. */
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
/* Joined brush ribbon: endpoint normals include the bounded miter scale.
 * The existing graph-line instance layout stays unchanged. */
void renderer_draw_stroke_segment(Renderer *r,f32 x0,f32 y0,f32 x1,f32 y1,
                                 f32 nx0,f32 ny0,f32 nx1,f32 ny1,f32 thickness,Color color);
void renderer_prepare_strokes(Renderer *r);

/* Bordered variant — a single SDF instance instead of an outer rrect for
 * the border and an inner rrect for the fill. The shader paints
 * `border_color` in the outermost `border_width` pixels and `fill`
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
 * Metal-only — OpenGL backend is a no-op (blur falls back to plain rrect).
 * NOTE: currently no call sites, and the CAMetalLayer runs framebufferOnly
 * (drawables can't be blit sources) — capture no-ops and the panel falls
 * back to a flat tint. To re-enable blur, set framebufferOnly = NO in
 * platform_macos.m. */
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

/* Rounded rect that KEEPS the framebuffer's alpha, so a translucent window
 * (reduced opacity, or a backdrop blur) shows through the shape exactly as
 * much as it shows through the frame's clear.
 *
 * It needs its own pass because the rrect pipeline blends alpha as a real
 * "over" (One / 1-SrcAlpha): ANY fill raises the alpha the compositor sees, so
 * there is no source alpha that leaves it where the clear put it. Drawing the
 * active tab pill at the window's own 0.79 over a 0.79 clear lands at 0.955 —
 * the blur dies under the pill no matter how translucent the colour looks, and
 * the pill reads as a solid slab floating on a frosted bar. This variant hands
 * the flush a pipeline whose alpha factors are (Zero, One), i.e. the alpha
 * channel is left as the clear wrote it.
 *
 * The fill is premultiplied by that alpha HERE, on the linear values, because
 * the framebuffer is premultiplied and rgb must never exceed its own alpha —
 * skipping it puts a colour in there too bright for its alpha and the shape
 * composites lighter than it should (the same error renderer_set_clear_color
 * documents for the clear).
 *
 * Self-flushing: the pipeline state differs from the batched one, so the mode
 * can never be left armed for somebody else's rrect. No shadow parameter — a
 * drop shadow darkens rgb without being able to raise alpha here, so it would
 * composite differently from every other shadow in the app; draw shadowed
 * surfaces with renderer_draw_rrect_bordered instead. Falls back to the
 * ordinary bordered draw on an opaque window: same pixels, one less pipeline
 * switch. */
static inline void renderer_begin_rrect_translucent_batch(Renderer *r) {
    renderer_flush_rrects(r);
    r->rrect_keep_dst_alpha = r->clear_color[3] < 0.999f;
}

static inline void renderer_draw_rrect_translucent_batched(Renderer *r,
    f32 x, f32 y, f32 w, f32 h,
    Color fill, Color border, f32 border_width,
    f32 r_tl, f32 r_tr, f32 r_br, f32 r_bl) {
    f32 fb_a = r->clear_color[3];
    if (fb_a >= 0.999f) {
        renderer_draw_rrect_bordered(r, x, y, w, h, fill, border, border_width,
                                     r_tl, r_tr, r_br, r_bl,
                                     0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }
    u32 before = r->rrect_batch_count;
    renderer_draw_rrect_bordered(r, x, y, w, h, fill, border, border_width,
                                 r_tl, r_tr, r_br, r_bl,
                                 0.0f, 0.0f, 0.0f, 0.0f);
    /* None when the backend fell back to a flat rect. A capacity flush cannot
     * happen in the prompt-card strips (<=4 entries), but guard it so this API
     * remains safe for future callers. */
    if (r->rrect_batch_count != before + 1) return;
    RRectInstance *ri = &r->rrect_batch[before];
    ri->r *= fb_a; ri->g *= fb_a; ri->b *= fb_a;
    ri->border_r *= fb_a; ri->border_g *= fb_a; ri->border_b *= fb_a;
}

static inline void renderer_end_rrect_translucent_batch(Renderer *r) {
    renderer_flush_rrects(r);
    r->rrect_keep_dst_alpha = false;
}

static inline void renderer_draw_rrect_translucent(Renderer *r,
    f32 x, f32 y, f32 w, f32 h,
    Color fill, Color border, f32 border_width,
    f32 r_tl, f32 r_tr, f32 r_br, f32 r_bl) {
    renderer_begin_rrect_translucent_batch(r);
    renderer_draw_rrect_translucent_batched(r, x, y, w, h, fill, border,
                                             border_width,
                                             r_tl, r_tr, r_br, r_bl);
    renderer_end_rrect_translucent_batch(r);
}

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
/* For renderer batches that prepare queued atlas writes before issuing draws.
 * cell_w == 0 selects the normal cell raster. Other callers use the gated API. */
bool font_get_glyph_rect_for_batch(FontAtlas *atlas, u32 cp, i32 cell_w, FontGlyphRect *out);
i32 font_prepare_ligature(FontAtlas *atlas, const u32 *cps, i32 count);
i32 font_prepare_ligature_run(FontAtlas *atlas, FontLigatureRun *run, i32 at, i32 count);
void renderer_flush_glyphs(Renderer *r);

/* bulk append pre-built instances (flushes mid-append if cap hit).
 * Used by the per-row render cache so non-dirty rows become a single memcpy.
 * Color channels in these instances must already be linear RGB. */
void renderer_append_rects(Renderer *r, const RectInstance *src, u32 count);
void renderer_append_glyphs(Renderer *r, const GlyphInstance *src, u32 count);
/* Rows are in pane-local pixels; the current draw origin is applied on GPU. */
bool renderer_draw_retained_rows(Renderer *r, RendererRetainedBatch **batch,
                                 u64 revision, const RendererRows *rows);
void renderer_retained_stats(const Renderer *r, RendererRetainedStats *out);
void renderer_set_draw_origin(Renderer *r, f32 x, f32 y);

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
/* renderer_set_gpu() returns nothing, and a renderer it could not set up draws
 * nothing: ASK. False = no backend; renderer_gpu_error() then says why, in a
 * sentence fit to show the user (never empty after a failure). The OpenGL
 * backend has no such step and always answers true. */
bool renderer_gpu_ready(const Renderer *r);
const char *renderer_gpu_error(void);
void renderer_set_clear_color(Renderer *r, f32 red, f32 green, f32 blue, f32 alpha);

/* Background image */
bool renderer_load_background_image(Renderer *r, const char *path, void *gpu_device);
/* Main-thread upload of caller-owned pixels; no file revision is inferred.
 * The buffer only needs to remain alive for this call. */
bool renderer_load_background_rgba(Renderer *r, const char *path,
                                   const u8 *pixels, i32 w, i32 h,
                                   void *gpu_device);
/* A decoder's captured revision enables sharing on Metal. Reject a stale
 * revision without replacing the current background, on both backends. */
bool renderer_load_background_rgba_versioned(Renderer *r, const char *path,
                                             const u8 *pixels, i32 w, i32 h,
                                             void *gpu_device, const FileStamp *source);
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

/* Same, but the texel is multiplied by `tint` in the shader.
 *
 * This is the path for white premultiplied MASKS (the Lucide icon pipeline):
 * the upload stays colour-independent, so one texture per (image, size) serves
 * every colour the same frame asks for. Recolouring by re-uploading under a
 * bumped `cache_generation` cannot do that — the cache rewrites the shared
 * texture in place, and because draws are only *encoded* at call time, every
 * already-encoded draw of that image silently picks up the last colour
 * written. That is a Metal-only hazard (GL orders texture writes against
 * draws).
 *
 * Pass a fully-opaque white tint to get renderer_draw_image_cached's
 * behaviour. `tint.a` scales the whole image's coverage. */
void renderer_draw_image_tinted(Renderer *r, const void *cache_key, u64 cache_generation,
                                const u8 *pixels, i32 img_w, i32 img_h,
                                f32 x, f32 y, f32 draw_w, f32 draw_h,
                                Color tint);

/* Block until GPU work already committed by this renderer completes. Returns true
 * only when the drain completed successfully. Use it when the CPU must rewrite or
 * destroy a resource that in-flight GPU work may still read; it is not a per-frame
 * operation.
 *
 * Atlas shelf reclamation uses renderer_gpu_idle instead. Reclaim must remain
 * bounded and can be abandoned if the renderer does not become idle.
 * font_atlas_replace must wait because it destroys the old texture while frames may
 * still be sampling it.
 *
 * The return value indicates whether synchronization completed. If the drain
 * command buffer could not be created or completed successfully, return false and
 * do not reuse the atlas space.
 *
 * Both backends implement this function, so it is declared outside the USE_METAL
 * guard. */
bool renderer_gpu_drain(Renderer *r);

/* Is every frame this renderer submitted FINISHED on the GPU? Never blocks.
 *
 * The atlas shelf reclaim's only safe barrier, and the reason it is "idle" rather
 * than a snapshot mark: a mark taken once and re-asked on later turns says
 * "everything submitted BEFORE the mark is done", while frames submitted AFTER it
 * keep sampling the same atlas. Asked at the instant of the reclaim, with no
 * encoder open, `idle` means there is no GPU reader of the atlas at all, which is
 * the property the reclaim actually requires.
 *
 * MAIN THREAD ONLY. The submitted count is written from the frame path on this
 * thread, so no concurrent submission can slip between the reads; the completion
 * counts are atomics written from a Metal thread and are read conservatively (an
 * under-read answers "not idle", never a false idle).
 *
 * true with no GPU behind the renderer — nothing can be reading anything. */
bool renderer_gpu_idle(Renderer *r);

/* A previously unavailable drawable has arrived. Does not consume it and
 * does not authorize bypassing display pacing or GPU-error backoff. */
bool renderer_drawable_ready(Renderer *r);

/* Frames this renderer submitted that FINISHED, split by outcome, plus the run of
 * consecutive failures ending at the most recent completion. Any pointer may be
 * NULL. `ok` and `failed` are monotonic; `fail_run` is not — a success resets it.
 *
 * `failed` exists because a frame whose GPU work errored never reached the screen,
 * so the window owes a repaint. `ok` exists because SUBMITTING a frame is not
 * evidence that one succeeded — a retry policy that treats it as evidence resets
 * its own bound every time a completion is a poll late.
 *
 * `fail_run` exists because THE TOTALS DO NOT CARRY THE ORDER. A poll that sees
 * both move learns that a success and a failure both landed and not which came
 * last. Only the completion handler knows the order, so it keeps the run and this
 * publishes it. The three are read under one lock: a fresh `failed` paired with a
 * stale `fail_run` would be a guess again.
 *
 * OpenGL has no asynchronous per-submission outcome to report: it counts frames
 * whose fence has signalled as `ok` and always answers 0 for `failed` and 0 for
 * `fail_run`. Those zeroes mean "this backend cannot answer", NOT "the GL path
 * never fails a frame". */
void renderer_gpu_frame_counts(Renderer *r, u64 *out_ok, u64 *out_failed,
                               u32 *out_fail_run);

#ifdef USE_METAL
/* Metal atlas helpers (called by font.c) */
void renderer_metal_set_atlas(Renderer *r, void *texture);
/* Upload data immediately into the specified region of a Metal texture. Shared
 * textures use replaceRegion; private textures use a blit on a temporary queue
 * that is waited on. src_bytes must cover w * h * bytes-per-texel using the
 * destination texture's pixel format; reject the upload otherwise.
 *
 * Before a renderer is attached, atlas initialization uses this immediate
 * path. After attachment, glyph uploads use renderer_atlas_upload so they are
 * ordered behind submitted frames. */
void renderer_metal_upload_texture(void *texture, i32 x, i32 y, i32 w, i32 h,
                                   const void *data, usize src_bytes);
/* Read a texture region into out using a row pitch of w * bytes_per_pixel.
 * The operation waits for GPU completion. */
bool renderer_metal_texture_readback(void *texture, i32 x, i32 y, i32 w, i32 h,
                                     void *out, usize out_bytes);
/* Zero a texture or copy the top-left w by h region to another texture
 * synchronously on the immediate queue. Use this for an atlas with no
 * renderer attached, including initialization before its first frame. An
 * attached atlas publishes texture replacement through
 * renderer_atlas_texture_replaced so the frame queue can order the
 * operation. */
void renderer_metal_texture_clear_sync(void *texture);
void renderer_metal_texture_copy_sync(void *src, void *dst, i32 w, i32 h);
#endif

/* =========================================================================
 * Backend atlas upload interface (both backends implement it)
 * =========================================================================
 *
 * `uploader` is FontAtlas.gpu_uploader — the renderer the atlas belongs to.
 *
 * renderer_atlas_upload records a region write. On Metal the bytes are COPIED
 * into a renderer-owned staging buffer and blitted into the texture at the
 * next frame's preparation, in GPU order behind every frame already
 * submitted; the caller may free `data` on return. `content_version` is the
 * atlas content version the write belongs to, and is what gpu_ready_version
 * advances to once the frame carrying it has been encoded. With a NULL
 * uploader the write happens immediately (headless / pre-frame seeding).
 * Returns false when NOTHING was taken — a staging allocation failed, the
 * region or the source did not fit — and font.c then leaves the glyph
 * uncached so the content version cannot claim texels the GPU will never
 * hold.
 *
 * renderer_atlas_texture_replaced announces that the atlas of `kind` now
 * lives in `new_tex` and that `copy_w x copy_h` texels of `old_tex` (the
 * top-left block, 0 x 0 for a reset) are its starting content. The frame in
 * flight keeps sampling `old_tex`; the next prepared frame copies, binds
 * `new_tex` and retires `old_tex` once every frame that sampled it has
 * completed. Uploads recorded after this call target `new_tex`. */
bool renderer_atlas_upload(void *uploader, void *texture, i32 x, i32 y, i32 w, i32 h,
                           const void *data, usize src_bytes, u32 content_version);
void renderer_atlas_texture_replaced(void *uploader, i32 kind, void *old_tex,
                                     void *new_tex, i32 copy_w, i32 copy_h);
/* Zero a region of an atlas texture — the shelf reclaim's clear — as a queued
 * op that stages NO bytes (the Metal backend blits strips of a shared zero
 * buffer). Same ordering and readiness rules as renderer_atlas_upload. */
bool renderer_atlas_clear(void *uploader, void *texture, i32 x, i32 y, i32 w, i32 h,
                          u32 content_version);
/* What the backend has done with the queue, for the probes and the tests:
 * records queued, bytes queued, blit encoders opened, records coalesced away,
 * staging high-water (bytes) and generations retired. Monotonic. */
typedef struct {
    u64 records, bytes, blits, coalesced, staging_high_water, retired,
        requeued, prepare_failures, dropped;
} RendererAtlasStats;
void renderer_atlas_stats(const Renderer *r, RendererAtlasStats *out);

#endif /* RENDERER_H */
