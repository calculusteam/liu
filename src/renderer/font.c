/*
 * Liu — font atlas with full Unicode support
 * Dynamic glyph cache: rasterizes glyphs on demand into a large atlas.
 * Supports ASCII, Latin Extended, Box Drawing, Block Elements, Braille,
 * Powerline, Arrows, Math symbols, and any other Unicode in the font.
 */
#include "renderer/renderer.h"
#include "core/mem_budget.h"
#include "renderer/emoji_bitmap.h"
#include <limits.h>
#include <errno.h>
#include "renderer/sfnt_probe.h"
#include "renderer/sprite_draw.h"
#include "core/checked.h"
#include "core/utf8.h"
#include "core/string_utils.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <sys/stat.h>
#include <strings.h>
#include <pthread.h>
#include <stdatomic.h>

#ifdef PLATFORM_MACOS
    #ifdef USE_METAL
        #import <Metal/Metal.h>
        /* The GL format tokens travel through font_gpu_upload's signature
         * on both backends; on Metal they are carried, never used. */
        #ifndef GL_RED
        #define GL_RED  0x1903
        #endif
        #ifndef GL_RGBA
        #define GL_RGBA 0x1908
        #endif
    #else
        #include <OpenGL/gl3.h>
    #endif
    #include <sys/mman.h>
    #include <fcntl.h>
    #include <unistd.h>
    #include <CoreText/CoreText.h>
    #include <CoreGraphics/CoreGraphics.h>
#elif defined(PLATFORM_LINUX)
    #include <GL/gl.h>
    #include <sys/mman.h>
    #include <fcntl.h>
    #include <unistd.h>
    #include <dirent.h>   /* system fallback discovery walks the font roots */
#else
    #include <GL/gl.h>
#endif
#if !defined(__APPLE__) && !defined(GL_VERSION_1_5)
    #include <GL/glext.h>   /* GL_R8/GL_CLAMP_TO_EDGE: Windows' gl.h is 1.1-only */
#endif

#ifndef ATLAS_BPP
#define ATLAS_BPP 1
#endif

/* ATLAS_BPP is the stride every CPU-side mirror, upload and readback in this
 * file packs with; Metal derives its own stride from the texture's pixel
 * format, which is MTLPixelFormatR8Unorm at both creation sites. The two are
 * separately maintained and must agree: a mismatch makes `getBytes:` above
 * font_ensure_atlas_room write past `new_bitmap`, and would make an upload read
 * past the caller's glyph buffer (renderer_metal_upload_texture refuses that
 * outright, but the readback has no such guard). Pin it here rather than
 * discovering the drift as a heap corruption. */
_Static_assert(ATLAS_BPP == 1,
               "glyph atlas stride must match the R8Unorm texture format");

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

const char *font_user_dir(void);

/* =========================================================================
 * Dynamic glyph cache
 * ========================================================================= */

/* THE CACHE HAS TO HOLD A SCREENFUL, and this number is derived from that.
 *
 * Eviction drops the coldest fifth once cache_used reaches the high water
 * below. When the working set is LARGER than the table, the glyphs it evicts
 * are still on screen, and an unrasterised glyph draws as a SPACE, so those
 * cells stay blank for as long as the text is there. That is not a slow path,
 * it is missing text, and it is permanent: the next frame re-rasterises them,
 * the frame after that evicts them again. The usable size is a little under
 * the high water, because the pre-warmed box-drawing and block-element ranges
 * already occupy ~160 entries.
 *
 * 8 192 is bounded on both sides. It must EXCEED the largest plausible screen:
 * a maximised 2 560x1 664 window at 16pt@2x with a 19x42 cell holds ~2 613
 * wide CJK cells (two columns each), and a 6K panel roughly 2.4x that. And it
 * must not exceed what the grown atlas can hold, or the table would outrun the
 * pixels: 4096x4096 fits ~9 500 double-width glyphs.
 *
 * The per-atlas table starts at 512 entries and grows to this bound before
 * eviction. Large screens retain their capacity without charging every idle
 * window for the maximum table. Eviction scratch remains process-wide. */
#define GLYPH_CACHE_SIZE 8192
/* Initial logical packing canvas (R8). Portable/writable atlases keep room
 * for later glyphs, avoiding synchronous growth during the first multilingual
 * burst. Metal's shared immutable seed uploads only its used rows; new glyphs
 * already use a separate 512² page. The logical canvas is therefore not the
 * amount of permanent GPU storage reserved for a Metal window. */
static void font_atlas_reaccount(FontAtlas *atlas);

/* ---------------------------------------------------------------------------
 * Memory ledger. The atlases are among the largest fixed allocations in the
 * process: the alpha atlas starts at 1536x1536 R8 and doubles to 4096x4096
 * (16 MiB), the emoji atlas is RGBA and doubles 512 -> 2048 (16 MiB), and
 * each has a CPU mirror — permanent on the GL backend, freed after the bulk
 * upload on Metal.
 *
 * Rather than pair an add with a subtract at each of the create / grow /
 * upload / reset / destroy sites, this recomputes the atlas's true totals and
 * posts the DIFFERENCE from what it last reported. Call it after any change
 * to the four buffers; calling it twice is harmless, and forgetting it in one
 * branch costs accuracy until the next call rather than corrupting a counter.
 *
 * The FACE is deliberately not counted. It is process-shared across every
 * window (g_font_info / g_ct_font behind g_face_refs) and is a mapping of a
 * file Liu does not own; counting it per atlas would report one font N times.
 * ------------------------------------------------------------------------- */
static void font_atlas_reaccount(FontAtlas *atlas) {
    if (!atlas) return;
    usize cpu = 0, gpu = 0, px = 0, bytes = 0;

    if (atlas->atlas_width > 0 && atlas->atlas_height > 0 &&
        checked_mul_usize((usize)atlas->atlas_width,
                          (usize)atlas->atlas_height, &px) &&
        checked_mul_usize(px, ATLAS_BPP, &bytes)) {
        if (atlas->atlas_bitmap) cpu += bytes;
        if ((atlas->metal_texture && !atlas->alpha_shared) || atlas->texture_id) gpu += bytes;
    }
    if (atlas->color_atlas_w > 0 && atlas->color_atlas_h > 0 &&
        checked_mul_usize((usize)atlas->color_atlas_w,
                          (usize)atlas->color_atlas_h, &px) &&
        checked_mul_usize(px, 4u, &bytes)) {
        if (atlas->color_bitmap) cpu += bytes;
        if (atlas->color_texture || atlas->color_texture_gl) gpu += bytes;
    }

    if (cpu != atlas->accounted_cpu) {
        mem_budget_adjust(MEM_FONT_CPU, (isize)cpu - (isize)atlas->accounted_cpu);
        atlas->accounted_cpu = cpu;
    }
    if (gpu != atlas->accounted_gpu) {
        mem_budget_adjust(MEM_FONT_GPU, (isize)gpu - (isize)atlas->accounted_gpu);
        atlas->accounted_gpu = gpu;
    }
}

#define ATLAS_INIT_SIZE   1536
#define ATLAS_MAX_SIZE    4096

typedef struct {
    u32  codepoint;
    f32  x0, y0, x1, y1;      /* atlas TEXELS — see FontGlyphRect in renderer.h */
    u64  last_used_frame;     /* coarse LRU timestamp; bumped per frame drain */
    u32  content_version;     /* the atlas content version this glyph's texels
                              * were written under; drawable once the GPU has
                              * been prepared up to it (FontAtlas.gpu_ready_version) */
    u8   kind;                /* FontGlyphKind: which atlas the rect is in */
    bool valid;
    bool exhausted;           /* true = atlas was full when this cp was first
                              * requested; the rect is the space glyph's and the
                              * codepoint must NOT be re-rasterized every frame.
                              * Cleared automatically when the table is wiped
                              * (memset to 0) on font reload / eviction rebuild. */
} CachedGlyph;

/* Hash table for fast glyph lookup. Open-addressed with linear probing; no
 * tombstones (lookup stops on first empty slot). Eviction therefore must
 * rebuild the table — see cache_evict_cold().
 *
 * NOTE: The atlas texture itself is a single-allocation backing store and is
 * NOT LRU'd here — only the lookup table. After a font-size change the atlas
 * may still hold stale glyph pixels at unused UVs, but those pixels cost
 * nothing at runtime; reclaiming them would require atlas repacking, which is
 * out of scope. */

/* ---- Ligature cache entry ------------------------------------------------
 * Declared up here (the cache itself is documented at its use site further
 * down) because FontAtlasPriv embeds it. The SIZE is the most the cache grows
 * to (lig_cache_resize), 8192 entries of 52 bytes: one screenful of
 * punctuation-dense rows asks a few thousand candidates, and a smaller cache
 * re-shapes much of the same screen when it is built again. */
#define LIGATURE_CACHE_SIZE 8192
#define LIGATURE_MAX_LEN    4

typedef struct {
    u32  codepoints[LIGATURE_MAX_LEN]; /* codepoint sequence */
    u8   count;                        /* number of chars */
    u8   width;                        /* ligature width in cells (0 = not a ligature) */
    bool valid;                        /* slot occupied */
    bool negative;                     /* true = known NOT to be a ligature */
    bool ctx;                          /* a run's decision: valid only between ctx_l and ctx_r */
    f32  x0, y0, x1, y1;              /* atlas texels (only meaningful if width > 0) */
    u32  content_version;             /* as CachedGlyph.content_version */
    u32  ctx_l, ctx_r;                 /* the cells either side in that run (0 = its edge) */
} LigatureEntry;

/* Every time a shaper was asked about ligatures — a candidate on its own or a
 * whole run (font_check_ligature_run). Main thread only, like the shaping. */
static u64 g_lig_shapings = 0;

/* ---- Async raster worker rings ------------------------------------------
 * Also hoisted: one worker per atlas, so its state lives in FontAtlasPriv. */
#define RASTER_REQ_CAP  512
#define RASTER_DONE_CAP 512
#define PENDING_SET_CAP 2048

/* Captured on the UI thread: workers never consult the mutable cluster table
 * or derive geometry from a handle after its request has been queued. */
typedef struct {
    u32 cp, base;
    u32 style;     /* FONT_STYLE_* bits of cp; cp itself is the cache key */
    i32 color_cells;   /* cells a colour glyph spans: 2, or 1 for a one-column emoji */
    i32 w, h;
    u32 text_len;
    u8 text[UTF8_CLUSTER_MAX * 4];
    bool color, cluster;
} RasterRequest;

typedef struct {
    u32  cp;
    u8  *bitmap;     /* malloc'd, owned by main thread after dequeue */
    i32  w, h;
    bool ok;
    u8 kind;
} RasterCompletion;

typedef struct {
    pthread_t       thread;
    pthread_mutex_t req_mu;
    pthread_cond_t  req_cv;
    pthread_mutex_t done_mu;
    pthread_cond_t  done_cv;   /* signalled when the main thread drains done_ring */
    RasterRequest   req_ring[RASTER_REQ_CAP];
    i32             req_head, req_tail;   /* ring indices */
    RasterCompletion done_ring[RASTER_DONE_CAP];
    i32             done_head, done_tail;   /* ring indices (under done_mu) */
    /* main-thread-only set to dedup in-flight requests. 0 = empty slot. */
    u32             pending_set[PENDING_SET_CAP];
    i32             pending_count;
    bool            mu_ready;  /* pthread objects initialised (heap-allocated) */
    bool            started;
    atomic_bool     stop;
    u32             completed_version; /* bumped when new glyphs land */
} RasterState;

/* ---- Everything ONE atlas owns -------------------------------------------
 *
 * Multi-window: each AppState owns a Renderer, which owns a FontAtlas and its
 * own GPU atlas texture. A glyph cache without atlas identity is therefore a
 * cross-window corruption engine in both directions — window A packs U+2500 at
 * rectangle P and records UV(P); window B "hits" that entry and samples P in
 * *its* texture (blank or some unrelated glyph), and B's own packing at the
 * shared cursor overwrites coordinates A's row caches still reference. Keep it
 * next to the texture it describes. Tables grow independently on demand. */
typedef struct FontAtlasPriv {
    CachedGlyph  *cache;
    u32           cache_cap;
    u32           cache_used;    /* count of valid slots */
    u64           glyph_frame;   /* LRU clock, bumped per drain */
    /* Set this flag when a lookup returns the space placeholder while
     * asynchronous rasterization is pending. The caller uses it to invalidate
     * rows that need a redraw. Atlas-exhaustion placeholders do not set it
     * because no raster completion will replace them. */
    bool          served_placeholder;
    LigatureEntry *lig;
    u32           lig_cap, lig_used;
    /* Atlas packing cursor: next free x, current row y, tallest glyph in row. */
    i32           pack_x, pack_y, pack_row_h;
    /* Where the shelf may be rewound to. Captured after the pre-warm, so a
     * reclamation keeps ASCII, box drawing and block elements (which are
     * addressed through atlas->glyphs[] rather than the hash cache, and are
     * what almost every frame draws) and reclaims only the area on-demand
     * glyphs took. */
    i32           pack_reset_x, pack_reset_y, pack_reset_row_h;
    bool          pack_reset_valid;

    /* The three atlas versions — see "Atlas synchronisation contract" in
     * renderer.h. content_version: a texel was written. extent_version: the
     * texture's size changed (the texel rects did not). placement_generation:
     * glyphs MOVED (a shelf reclaim, a colour-atlas reset, an atlas
     * replacement) — every cached rect is stale. A grow is not a placement
     * change: with texel coordinates it moves nothing a cache holds. This is
     * NOT raster.completed_version: that one says "a new glyph landed", and
     * its consumer deliberately repairs only the rows that drew a
     * placeholder, which is exactly the set a placement change does NOT
     * belong to. */
    u32           content_version;
    u32           extent_version;
    u32           placement_generation;
    /* Writes the renderer refused (staging exhausted, a region that did not
     * fit). Each left its glyph uncached, to be rasterised again on the next
     * lookup; counted so a probe can tell a refused write from a lost one. */
    u32           upload_refused;

    /* Called before shelf space is returned for reuse. The Metal upload path
     * may leave earlier frames sampling these cells, so the renderer must
     * drain pending GPU work before reclamation. font.c does not own the
     * renderer; the app installs this callback. A null callback means no GPU
     * uses the atlas. */
    bool        (*gpu_sync)(void *ctx);
    void         *gpu_sync_ctx;

    /* "Is every frame this renderer submitted finished?" — the only predicate the
     * shelf reclaim may act on, and it has to be asked at the INSTANT of the
     * reclaim: a mark taken earlier and asked again on later turns says nothing
     * about the frames submitted in between, and those frames sample the same
     * atlas. */
    bool        (*gpu_idle)(void *ctx);

    /* Flush emitted glyph instances before an atlas grow changes its
     * geometry. Their UV coordinates are relative to the current atlas;
     * drawing them after the grow would use mismatched coordinates. A null
     * callback is a no-op when no GPU encoder is active. */
    void        (*gpu_flush)(void *ctx);

    /* A reclaim that has been ASKED FOR but not performed, because the GPU
     * cannot be stopped safely from where it was asked. See
     * atlas_reclaim_shelf() and font_atlas_service_reclaim(). */
    bool          reclaim_wanted;
    /* Turns spent waiting for the GPU to go idle, and when frame suppression
     * started. The bound on suppression is a WALL CLOCK because an attempt count is
     * not a measure of time — see FONT_RECLAIM_QUIESCE_SEC.
     *
     * `reclaim_quiescing` is a separate flag rather than `since == 0.0` meaning "not
     * started": 0.0 is a LEGAL reading of the clock this is handed
     * (platform_time_sec() is seconds since init), so the sentinel would overload a
     * value it is supposed to distinguish from. For that to misfire the clock would
     * have to read exactly 0.0 on the turn quiesce begins, at least eight service
     * turns after a request; the flag costs a bool and removes the question. */
    u32           reclaim_turns;
    bool          reclaim_quiescing;
    f64           reclaim_quiesce_since;
    /* Reclaims dropped at that bound. Published, because a non-zero count means
     * some codepoints kept a placeholder they should not have. */
    u32           reclaim_abandons;
    /* Glyphs packed since the last reclamation, and how many reclamations have
     * happened. A reset that immediately re-exhausts means the working set does
     * not fit the atlas at all, and resetting again would thrash the
     * rasteriser — so the sentinel comes back for that episode. */
    u32           packed_since_reset;
    u32           atlas_resets;
    RasterState   raster;

    /* The face identity this atlas was BUILT with. Face state (mmap,
     * stbtt_fontinfo, CTFont, fallback chain, every derived scale) is
     * process-shared, and font_atlas_create REPLACES it the moment another
     * window asks for a different size or DPI — a second display at a
     * different backing scale is enough. The outlines survive that; the
     * SCALES do not, so an atlas that outlives the swap rasterises its next
     * glyph at the other window's size inside its own cell box. */
    char face_path[1024];
    f32  face_size, face_dpi, face_weight, face_line_h;
    f32  face_width_scale, face_height_scale;
    bool face_norm;
    u8   face_smooth;                /* FONT_SMOOTH_* */
    /* The face options it was built with (font_set_face_options). */
    char face_features[256], face_variations[256], face_cpmap[512];
} FontAtlasPriv;

static inline FontAtlasPriv *fa_priv(const FontAtlas *a) {
    return a ? (FontAtlasPriv *)a->priv : NULL;
}

void font_atlas_cache_stats(const FontAtlas *atlas,FontCacheStats *stats) {
    if(!stats)return;
    const FontAtlasPriv *p=fa_priv(atlas);*stats=(FontCacheStats){0};if(!p)return;
    *stats=(FontCacheStats){p->cache_cap,p->cache_used,p->lig_cap,p->lig_used,
        sizeof(*p)+(usize)p->cache_cap*sizeof(*p->cache)+(usize)p->lig_cap*sizeof(*p->lig)};
}

#ifdef USE_METAL
#include "renderer/font_shared_seed.h"
#endif

/* Mirror an R8 atlas upload into the CPU copy, when one exists.
 *
 * Metal keeps a mirror and writes it alongside every upload. The GL backend
 * normally has none — the texture is the atlas — but it does hold one in two
 * cases, and without this both are silently wrong:
 *
 *   after a growth, where the mirror is kept precisely to seed the NEXT
 *   growth. A mirror left stale would make the second grow re-upload a
 *   snapshot taken at the first, dropping every glyph rasterised in between;
 *
 *   headless, where this file is compiled with no GL context and glyph
 *   pixels are read back out of the mirror.
 *
 * No-op when there is no mirror, which is the steady state of a real session. */
static void atlas_mirror_r8(FontAtlas *atlas, i32 ax, i32 ay,
                            i32 w, i32 h, const u8 *src) {
    if (!atlas || !atlas->atlas_bitmap || !src) return;
    for (i32 row = 0; row < h; row++)
        memcpy(atlas->atlas_bitmap +
                   ((usize)(ay + row) * (usize)atlas->atlas_width + (usize)ax) * ATLAS_BPP,
               src + (usize)row * (usize)w, (usize)w * ATLAS_BPP);
}

static stbtt_fontinfo g_font_info;
static u8 *g_font_data = NULL;
static usize g_font_data_size = 0;
/* Reference count on the PROCESS-SHARED face state below (the mmap that
 * g_font_info points into, the CoreText font/context, and the fallback chain).
 * font_atlas_destroy() runs on every window close; unmapping the font file
 * there unconditionally would leave the surviving windows rasterising their
 * next box-drawing/nerd-font/CJK glyph against freed memory. Only tear the
 * face down when the LAST atlas goes away. */
static i32 g_face_refs = 0;

/* Guards MUTATION of that shared face (the mmap swap, stbtt_InitFont, the
 * CoreText objects, free_fallbacks) against the raster workers READING it.
 *
 * Each atlas runs its own raster worker, so window B's worker keeps
 * rasterising straight through window A's font-size reload, and a reload both
 * unmaps the file g_font_info points into and rebuilds the fallback chain.
 *
 * OUTERMOST lock: g_face_mu -> g_ct_mu -> g_fb_mu, never the reverse. Only the
 * worker-reachable raster entry point takes it on the read side; the main
 * thread's own rasterisers do not need it, because the only writer (atlas
 * create/destroy) is the main thread itself. */
static pthread_mutex_t g_face_mu = PTHREAD_MUTEX_INITIALIZER;

/* One immutable CPU prewarm template for the currently bound face. Retained
 * only with multiple live atlases; no extra cache allocation for one window.
 * Invalidation and access share g_face_mu with face/fallback mutations. */
typedef struct {
    usize bytes, pixel_bytes;
    i32 width, height, used_height, pack_x, pack_y, pack_row_h;
    u32 cache_used, content_version, extent_version;
    u8 glyphs[sizeof(((FontAtlas *)0)->glyphs)];
    CachedGlyph cache[512];
    u8 pixels[];
} FontPrewarm;
static FontPrewarm *g_prewarm;
static u64 g_prewarm_hits, g_prewarm_misses;
static void font_prewarm_clear_locked(void) {
    if(!g_prewarm)return;
    mem_budget_transient_adjust(MEM_FONT_CPU,-(isize)g_prewarm->bytes);
    free(g_prewarm);g_prewarm=NULL;
}
void font_prewarm_stats(FontPrewarmStats *stats) {
    if(!stats)return;
    pthread_mutex_lock(&g_face_mu);
    *stats=(FontPrewarmStats){g_prewarm_hits,g_prewarm_misses,g_prewarm?g_prewarm->bytes:0};
    pthread_mutex_unlock(&g_face_mu);
}

/* Defined next to the face loader it calls: rebinds the shared face to the
 * identity one atlas was built with. Declared here because every raster below
 * takes g_face_mu and then calls it. */
static bool face_bind_locked(const FontAtlas *atlas);

/* Identity + derived metrics of the face currently loaded into the globals
 * above. A second window asking for the same font/size/DPI/weight/options
 * must NOT re-run the load: that re-maps the file and calls free_fallbacks()
 * while the first window's raster worker is walking the very chain being
 * freed, and it re-derives metrics the first window's atlas is already packed
 * against. Matching parameters therefore just take a reference and copy the
 * metrics back out. A genuine change (font-size / family reload) still falls
 * through to the full load path. */
static struct {
    char path[1024];
    f32  size, dpi, weight, line_height;
    f32  width_scale, height_scale;
    bool normalize;
    u8   smoothing;                  /* FONT_SMOOTH_* */
    char features[256], variations[256], cpmap[512];
    /* Produced by the load; replayed into a reusing atlas. */
    f32  cell_w, cell_h, ascent, descent, line_box;
    /* Decoration geometry in pixels from the top of the cell (see
     * face_line_metrics) and the stroke procedural glyphs draw with. */
    f32  underline_y, underline_h, strike_y, strike_h;
    i32  line_px;
} g_face;
static f32 g_font_scale = 0;
static f32 g_font_ascent = 0;
/* Uniform horizontal pen offset applied to every glyph, so a face whose ink
 * reaches past its own advance (or starts left of the origin — Anonymous Pro's
 * 'W' has a -0.9 px left bearing) is centred in the cell instead of losing one
 * side to the cell edge. Uniform across the face, so the monospace grid is
 * untouched. */
static f32 g_glyph_pen_x = 0;
/* Cell width the current atlas was built at. Kept next to the pen because the
 * per-glyph centring below needs both, and the atlas struct is not reachable
 * from the rasterisers. */
static f32 g_cell_width  = 0;
/* The face's own widest-ASCII advance, BEFORE the cell rounded it (JetBrains
 * Mono is 19.2 px where the cell is 19). A single glyph never needs this — it
 * is drawn at its own cell origin — but a shaped RUN does: the shaper lays a
 * ligature out on the natural advance while the grid marches on the rounded
 * cell, so the two drift apart by (advance - cell) per column. Recorded by
 * face_load_locked next to the rounding that loses it. */
static f32 g_face_adv_px = 0;
/* Cap height of the loaded face in pixels, and the height an icon from a
 * symbols font is fitted to: two parts cap height to one part line box, so an
 * icon stands a little taller than a capital and stays clear of the rows
 * above and below. Set by face_load_locked. */
static f32 g_cap_px = 0;
static f32 g_icon_h_px = 0;
/* The same for an icon that has the blank cell after it as well: about one
 * em, half way from the cap height to the line box. */
static f32 g_icon2_h_px = 0;

/* Horizontal pen for ONE glyph.
 *
 * In a monospace face every ASCII advance equals the cell, the slack is zero,
 * and this is exactly g_glyph_pen_x — nothing moves.
 *
 * A PROPORTIONAL face is the case this exists for. The cell is sized to the
 * widest ASCII advance (see stb_max_ascii_advance), so in SF Pro — which macOS
 * ships and which is offered in Settings — 'W' is 1892 units while 'i' is 422.
 * Drawing every glyph at the same pen would leave the narrow ones jammed
 * against the left edge of a cell four times their width, reading as though
 * every word had been letter-spaced. Splitting each glyph's slack evenly
 * centres it in its cell instead. The grid is still fixed-pitch — that is what
 * a terminal is — but the rhythm is even rather than lopsided. */
/* `mul` is the face scale this particular raster runs at: 1.0 for every
 * terminal cell, >1 while filling a sized cache entry for large UI text. The
 * pen and the cell both live in atlas-cell units, so both scale with it. */
/* `box_px` is the width of the raster this glyph is being drawn into: one cell
 * normally, TWO for a double-width glyph (CJK, fullwidth). Passing 0 falls
 * back to the single-cell width — centring a CJK glyph against one cell leaves
 * it hugging the left edge of a box twice that wide, which reads as a gap
 * after every ideograph. */
/* A fallback face's glyph WIDER than the cell is centred on it the same way,
 * in whole pixels so its stems keep the face's pixel phase: a symbols face
 * drawn at the text's size centres its glyph in an advance far wider than the
 * cell, and placed from the cell's left edge that glyph stands right of
 * centre. The primary face's own advance keeps its pen whatever the cell
 * spacing. */
static f32 font_glyph_pen_x_mb(f32 advance_px, f32 mul, f32 box_px) {
    f32 pen  = g_glyph_pen_x * mul;
    f32 cell = (box_px > 0.0f) ? box_px : g_cell_width * mul;
    if (!isfinite(advance_px) || advance_px <= 0.0f) return pen;
    f32 slack = cell - advance_px;
    if (slack > 0.5f) return pen + slack * 0.5f;
    if (slack < -1.0f && fabsf(advance_px - g_face_adv_px * mul) > 0.5f)
        return pen + roundf(slack * 0.5f);
    return pen;                                /* monospace: exactly zero */
}

/* Synthetic stroke width applied at glyph rasterisation. 0 = native weight,
 * >0 thickens. Set by font_atlas_create from Config.font_weight. */
static f32 g_font_weight = 0.0f;

/* The slant of a synthesised italic: x moves by this much per unit of height,
 * about the middle of the cell so the ink leans out of the box as little at the
 * top as at the bottom. */
#define SYNTH_ITALIC_SHEAR 0.2f

/* Stroke width, in target pixels, a synthesised bold adds around the outline
 * at pixel size `px`: a thirty-second of the size inks a regular face about as
 * heavily as its family's weight-700 instance does (x1.4). */
static f32 synth_bold_width(f32 px) {
    f32 w = px / 32.0f;
    return w < 0.4f ? 0.4f : w;
}


/* Font faces use different em proportions, so equal point sizes can produce
 * different cap heights and line boxes. Normalize each face against the
 * reference cap height, derive the line box from the requested pixel size, and
 * center the ink extent to avoid clipping. */
#define FONT_REF_CAP_RATIO   0.730f   /* JetBrains Mono cap / em */
#define FONT_REF_LINE_RATIO  1.320f   /* JetBrains Mono line box / em */
#define FONT_NORM_MIN_SCALE  0.85f
#define FONT_NORM_MAX_SCALE  1.25f

/* Options for the currently loaded face and options requested for the next
 * atlas are separate. Worker-side face binding reads the current options;
 * atlas creation copies the request while holding g_face_mu. Keeping these
 * values separate prevents another window's worker from overwriting a pending
 * request. */
static f32  g_opt_line_height = 1.0f;
static bool g_opt_normalize   = true;
static u8   g_opt_smoothing   = FONT_SMOOTH_FULL;   /* FONT_SMOOTH_* */
/* The REQUEST. Written by font_set_metrics_options, consumed by
 * font_atlas_create. Both under g_face_mu, so no reader has to wonder. */
static f32  g_req_line_height = 1.0f;
static bool g_req_normalize   = true;
static FontSmoothing g_req_smoothing = FONT_SMOOTHING_ALWAYS;

#ifdef PLATFORM_MACOS
/* Core Graphics does not smooth text drawn in kCGTextFillStroke mode. With
 * smoothing on, a stroked glyph (a synthesised bold, a positive font weight)
 * would lose the dilation the glyphs around it get and draw no heavier than
 * they do, so its fill is drawn on its own first and the stroke over it. */
static bool glyph_stroke_drawn_apart(f32 stroke) {
    return stroke > 0.01f && g_opt_smoothing != FONT_SMOOTH_OFF;
}

/* The grey a glyph is filled and stroked with in the alpha-only raster
 * context. Only its alpha is written; the grey sets how far Core Graphics
 * dilates a smoothed outline: black is the light level, white the full one. */
static CGFloat glyph_fill_grey(void) {
    return g_opt_smoothing == FONT_SMOOTH_LIGHT ? 0.0 : 1.0;
}
#endif
/* Face options: OpenType features, variation axes and the codepoint-to-face
 * map, as AppConfig spells them. Request and loaded copy, like the metric
 * options above; both under g_face_mu. */
static char g_req_features[256], g_req_variations[256], g_req_cpmap[512];
static char g_opt_features[256], g_opt_variations[256], g_opt_cpmap[512];

/* =========================================================================
 * FreeType rasteriser (Linux)
 *
 * stb_truetype computes exact area coverage, which is a correct answer to the
 * wrong question: it has no hinting, so at a 16 px body size a vertical stem
 * whose outline falls between two pixel columns is rendered as two half-lit
 * columns rather than one solid one. Every stem in the face lands somewhere
 * different, and the result reads soft and uneven no matter how the coverage
 * is post-processed; the 1.43 shader gamma below cannot fully compensate.
 *
 * FreeType's light autohinter snaps stems and the baseline to the pixel grid
 * vertically while leaving horizontal metrics untouched, which is exactly the
 * trade a fixed-cell terminal wants: the glyphs get crisp, the grid does not
 * move. It is also where the user's fontconfig settings actually live, so the
 * hint style and antialias flags are read from there rather than invented
 * here — a machine configured for no antialiasing should get none from Liu.
 *
 * Optional: when the library is absent every one of these entry points fails
 * softly and the stb path below renders the glyph.
 * ========================================================================= */
#ifdef LIU_HAVE_FREETYPE
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#ifdef LIU_HAVE_HARFBUZZ
#include <hb.h>
#include <hb-ft.h>
#endif

static FT_Library g_ft_lib    = NULL;
static FT_Face    g_ft_face   = NULL;   /* primary face; fallbacks live in g_fallbacks[] */
static bool       g_ft_active = false;  /* a primary face is loaded and usable */
static FT_Int32   g_ft_load_flags  = FT_LOAD_TARGET_LIGHT;
static FT_Render_Mode g_ft_render_mode = FT_RENDER_MODE_LIGHT;

/* An FT_Face carries its own glyph slot and size state, so two threads
 * rendering through one face corrupt each other. The async raster worker and
 * the render thread both get here — serialise the whole load/render/copy
 * sequence, the same way the Core Text path serialises on g_ct_mu. */
static pthread_mutex_t g_ft_mu = PTHREAD_MUTEX_INITIALIZER;
#endif

/* Coverage gamma the glyph fragment shader should apply. Core Graphics font
 * smoothing already dilates the outline, so thickening a second time in the
 * shader just re-introduces the washed-out halo it was meant to fix. Without
 * smoothing — the option off, or the stb_truetype raster on Linux — linear
 * coverage renders skeletal and the 1/1.43 curve restores native weight.
 *
 * FreeType is the third case: a hinted raster already puts full coverage down
 * the middle of a stem, so raising the mid-tones on top of it only fattens
 * the edge that hinting just made crisp. Same reasoning as smoothing on
 * macOS, same answer — leave the coverage alone. */
/* PUBLISHED, not computed on demand. The inputs are face state — g_ft_active
 * and the loaded face's smoothing — and the reader is the RENDER PATH: both
 * backends call this on every glyph flush (renderer_metal.m's uniform compare
 * and write, renderer.c's glUniform1f). A raster worker rewrites both inputs
 * inside face_bind_locked, and on Linux ft_primary_load has a real window
 * where g_ft_active is observably false while the face is being rebuilt, so
 * reading them live is a race that flips the shader's coverage gamma between
 * 1.0 and 1.43 for a frame.
 *
 * Taking g_face_mu here is not an option — that is a process-wide lock on the
 * per-flush path, contended by every other window's worker. The value only
 * changes when the face changes, so face_load_locked computes it once under
 * the lock and stores it here; this is a plain atomic load.
 *
 * Initialised to what the defaults produce, for the flushes that can happen
 * before any face exists. */
#ifdef PLATFORM_MACOS
static _Atomic f32 g_text_gamma = 1.0f;    /* smoothing defaults on */
#else
static _Atomic f32 g_text_gamma = 1.43f;
#endif

f32 font_text_gamma(void) {
    return atomic_load_explicit(&g_text_gamma, memory_order_relaxed);
}

/* Recompute from the face that was just loaded. g_face_mu must be held. */
static void font_publish_text_gamma_locked(void) {
    f32 g;
#ifdef LIU_HAVE_FREETYPE
    if (g_ft_active) g = 1.0f;
    else
#endif
#ifdef PLATFORM_MACOS
    g = g_opt_smoothing != FONT_SMOOTH_OFF ? 1.0f : 1.43f;
#else
    g = 1.43f;
#endif
    atomic_store_explicit(&g_text_gamma, g, memory_order_relaxed);
}

/* Records what the NEXT font_atlas_create should build with. It deliberately
 * does not touch the loaded face's own options — see the g_opt_/g_req_ note.
 *
 * Takes g_face_mu because every reload site calls this between a destroy and a
 * create, i.e. with other windows' raster workers live. The lock is never held
 * by any caller (all of them are the destroy/set/create shape), so this cannot
 * self-deadlock. */
void font_set_face_options(const char *features, const char *variations,
                           const char *cp_map) {
    pthread_mutex_lock(&g_face_mu);
    snprintf(g_req_features, sizeof g_req_features, "%s", features ? features : "");
    snprintf(g_req_variations, sizeof g_req_variations, "%s", variations ? variations : "");
    snprintf(g_req_cpmap, sizeof g_req_cpmap, "%s", cp_map ? cp_map : "");
    pthread_mutex_unlock(&g_face_mu);
}

void font_set_metrics_options(f32 line_height, bool normalize, FontSmoothing smoothing) {
    pthread_mutex_lock(&g_face_mu);
    g_req_line_height = (isfinite(line_height) && line_height > 0.05f)
                      ? CLAMP(line_height, 0.5f, 4.0f) : 1.0f;
    g_req_normalize = normalize;
    g_req_smoothing = smoothing == FONT_SMOOTHING_NEVER || smoothing == FONT_SMOOTHING_ALWAYS
                    ? smoothing : FONT_SMOOTHING_AUTO;
    pthread_mutex_unlock(&g_face_mu);
}

u8 font_smoothing_level(FontSmoothing mode, f32 dpi_scale) {
    if (mode == FONT_SMOOTHING_NEVER) return FONT_SMOOTH_OFF;
    if (mode == FONT_SMOOTHING_ALWAYS) return FONT_SMOOTH_FULL;
    return dpi_scale >= 1.5f ? FONT_SMOOTH_OFF : FONT_SMOOTH_LIGHT;
}

#ifdef LIU_HAVE_FONTCONFIG
#include <fontconfig/fontconfig.h>
#endif

#ifdef LIU_HAVE_FREETYPE
/* Resolve hint style and antialiasing from fontconfig, i.e. from whatever the
 * user's desktop wrote there. Queried against "monospace" because that is the
 * pattern the terminal's own font stands in for, and because the answer
 * wanted is the machine-wide default rather than anything specific to the
 * file Liu happens to have loaded.
 *
 * Defaults if fontconfig is absent or says nothing: slight hinting, greyscale
 * antialiasing, which is what fontconfig itself ships. */
static void ft_read_render_prefs(void) {
    int  hintstyle = 1;          /* FC_HINT_SLIGHT */
    bool antialias = true, hinting = true;

#ifdef LIU_HAVE_FONTCONFIG
    if (FcInit()) {
        FcPattern *pat = FcNameParse((const FcChar8 *)"monospace");
        if (pat) {
            FcConfigSubstitute(NULL, pat, FcMatchPattern);
            FcDefaultSubstitute(pat);
            FcResult res;
            FcPattern *m = FcFontMatch(NULL, pat, &res);
            if (m) {
                int v; FcBool b;
                if (FcPatternGetInteger(m, FC_HINT_STYLE, 0, &v) == FcResultMatch)
                    hintstyle = v;
                if (FcPatternGetBool(m, FC_ANTIALIAS, 0, &b) == FcResultMatch)
                    antialias = (b == FcTrue);
                if (FcPatternGetBool(m, FC_HINTING, 0, &b) == FcResultMatch)
                    hinting = (b == FcTrue);
                FcPatternDestroy(m);
            }
            FcPatternDestroy(pat);
        }
    }
#endif

    if (!antialias) {
        /* One bit per pixel. Honoured rather than ignored: a user who turned
         * antialiasing off did it on purpose, just as with the hint style.
         */
        g_ft_load_flags   = FT_LOAD_TARGET_MONO;
        g_ft_render_mode  = FT_RENDER_MODE_MONO;
    } else if (!hinting || hintstyle == 0 /* FC_HINT_NONE */) {
        g_ft_load_flags   = FT_LOAD_NO_HINTING;
        g_ft_render_mode  = FT_RENDER_MODE_NORMAL;
    } else if (hintstyle == 1 /* FC_HINT_SLIGHT */) {
        /* Vertical-only hinting: stems and baselines snap to the grid, glyph
         * widths and side bearings do not move. The only mode a fixed-cell
         * grid can take without the columns drifting. */
        g_ft_load_flags   = FT_LOAD_TARGET_LIGHT;
        g_ft_render_mode  = FT_RENDER_MODE_LIGHT;
    } else {
        g_ft_load_flags   = FT_LOAD_TARGET_NORMAL;
        g_ft_render_mode  = FT_RENDER_MODE_NORMAL;
    }
}

/* Build an FT_Face over a font file already mapped for stb_truetype. Shares
 * the mapping rather than re-reading the file: FreeType only borrows the
 * buffer, so it must stay alive until FT_Done_Face, which is exactly the
 * lifetime unmap_font_file already gives it. */
static FT_Face ft_face_from_memory(const u8 *data, usize size) {
    if (!g_ft_lib || !data || size == 0) return NULL;
    FT_Face face = NULL;
    if (FT_New_Memory_Face(g_ft_lib, (const FT_Byte *)data, (FT_Long)size, 0,
                           &face) != 0)
        return NULL;
    /* Bitmap-only faces (NotoColorEmoji) have no outline to hint and no scaler
     * that FT_Set_Char_Size can drive to an arbitrary ppem; the colour-emoji
     * path handles those separately, so decline them here. */
    if (!FT_IS_SCALABLE(face)) { FT_Done_Face(face); return NULL; }
    return face;
}

static void ft_face_destroy(FT_Face *face) {
    if (face && *face) { FT_Done_Face(*face); *face = NULL; }
}

/* Drop the primary face. Must run before the mapping it borrows goes away —
 * every unmap of g_font_data is preceded by this. */
static void ft_primary_release(void) {
    ft_face_destroy(&g_ft_face);
    g_ft_active = false;
}

/* Open the primary face over the already-mapped font file, bringing the
 * library up on first use. Failure is not an error: g_ft_active stays false
 * and every glyph goes to stb, which is the pre-FreeType behaviour. */
static void ft_primary_load(void) {
    ft_primary_release();
    if (!g_ft_lib) {
        if (FT_Init_FreeType(&g_ft_lib) != 0) { g_ft_lib = NULL; return; }
        ft_read_render_prefs();
    }
    g_ft_face = ft_face_from_memory(g_font_data, g_font_data_size);
    g_ft_active = (g_ft_face != NULL);
}

#define FT_PRIMARY_RELEASE() ft_primary_release()
#else
#define FT_PRIMARY_RELEASE() ((void)0)
#endif /* LIU_HAVE_FREETYPE */

/* Forward declare MAX_FALLBACK_FONTS */
#ifdef PLATFORM_LINUX
/* The built-in basename list alone can fill 16 slots on a well-stocked
 * distribution, leaving fontconfig discovery nowhere to put what it finds.
 * Raised here only: macOS discovers lazily and one face at a time. */
#define MAX_FALLBACK_FONTS 32
#else
#define MAX_FALLBACK_FONTS 16
#endif

/* Serializes all fallback-font-chain state (g_fallbacks / g_fallback_count,
 * the CT fallbacks on macOS, g_cp_fb_cache) between the render thread and the
 * async raster worker. Both threads discover/append fallbacks, and the
 * unsynchronized check-then-act counter increments could push a count past
 * MAX_FALLBACK_FONTS (OOB array access) and leak font refs. This is always
 * the INNERMOST lock: a thread may hold g_ct_mu and then take g_fb_mu, never
 * the reverse. Lives outside the PLATFORM_MACOS block: the locking wrapper
 * find_font_for_glyph() is platform-independent. */
static pthread_mutex_t g_fb_mu = PTHREAD_MUTEX_INITIALIZER;

#ifdef PLATFORM_MACOS
/* Core Text font handle — native macOS text rendering */
static CTFontRef g_ct_font = NULL;
static CTFontRef g_ct_fallbacks[MAX_FALLBACK_FONTS];

/* Forward declarations for lazy-fallback plumbing defined further down. */
static bool load_next_lazy_fallback(u32 cp);
static i32 g_ct_fallback_count = 0;

/* Bold, italic and bold-italic faces of the primary, built on first use and
 * indexed by FONT_STYLE_* >> 28 (slot 0 is the regular face, never stored
 * here). A slot holds the real face when the family has one — a sibling file
 * next to the chosen one, or the weight axis of a variable face — and the
 * regular face otherwise, with g_ct_styled_synth saying what the rasteriser
 * has to add. Face state: written under g_face_mu, dropped with g_ct_font. */
#define STYLED_SYNTH_BOLD   1u
#define STYLED_SYNTH_ITALIC 2u
static CTFontRef g_ct_styled[4];
static u8        g_ct_styled_synth[4];
static bool      g_ct_styled_tried[4];

/* Supersampling factor. Core Text renders at OVERSAMPLE× the target cell size,
 * then a box filter downsamples into the atlas. Pure grayscale AA — LCD
 * subpixel filtering is intentionally OFF because:
 *   (1) On Retina the human eye can't resolve LCD ordering; macOS itself
 *       dropped system LCD smoothing in Mojave.
 *   (2) Mixing supersample with LCD subpixel + box downsample destroys the
 *       subpixel pattern and leaves coloured fringing without sharpening.
 *
 * Set to 1: box-averaging the exact area coverage of four subpixels *is* the
 * coverage of the parent pixel, so 2× + box downsample gives the same
 * coverage as a direct 1× raster for 4× the CoreGraphics rasterisation work
 * per glyph. Kept as a knob because the generic path below still handles any
 * factor. */
#define FONT_OVERSAMPLE 1

/* Pooled CGBitmapContext — avoids kernel alloc/dealloc per glyph.
 * Dimensions are OVERSAMPLE× the target cell; readback downsamples to R8.
 * Shared between the main thread and the async raster worker, so every draw
 * into it — glyph or cluster — happens under g_ct_mu. */
static pthread_mutex_t g_ct_mu = PTHREAD_MUTEX_INITIALIZER;
static CGContextRef g_glyph_ctx = NULL;
static u8          *g_glyph_buf = NULL;    /* OVERSAMPLE×, 1 byte/pixel (alpha) */
static i32          g_glyph_ctx_w = 0;     /* stored in target (1×) cells */
static i32          g_glyph_ctx_h = 0;

static CGContextRef get_glyph_context(i32 cw, i32 ch) {
    if (g_glyph_ctx && g_glyph_ctx_w == cw && g_glyph_ctx_h == ch)
        return g_glyph_ctx;
    if (g_glyph_ctx) CGContextRelease(g_glyph_ctx);
    g_glyph_ctx = NULL;
    g_glyph_ctx_w = 0;
    g_glyph_ctx_h = 0;
    free(g_glyph_buf);
    g_glyph_buf = NULL;
    i32 bw = cw * FONT_OVERSAMPLE;
    i32 bh = ch * FONT_OVERSAMPLE;
    g_glyph_buf = calloc((usize)bw, (usize)bh);
    if (!g_glyph_buf) return NULL;
    /* Alpha-only context: CG writes one byte per pixel = geometric glyph
     * coverage. No colour space, no gamma encoding involved → averaging in
     * the downsample is mathematically correct. */
    g_glyph_ctx = CGBitmapContextCreate(g_glyph_buf, (size_t)bw, (size_t)bh,
                                         8, (size_t)bw, NULL,
                                         (CGBitmapInfo)kCGImageAlphaOnly);
    if (g_glyph_ctx) {
        CGContextSetAllowsAntialiasing(g_glyph_ctx, true);
        CGContextSetShouldAntialias(g_glyph_ctx, true);
        /* Font smoothing = CoreGraphics' outline dilation, the thing that makes AppKit
         * text look solid instead of skeletal. In an alpha-only context it is a plain
         * geometric thickening (no LCD fringing — the RGB filter needs a colour
         * context), and it is what every native macOS text surface gets: more solid
         * stems and fewer grey in-between pixels.
         *
         * A coverage gamma in the fragment shader is no substitute: it raises the
         * *mid-tones*, widening the soft halo instead of filling the stem, so text reads
         * as washed out. With smoothing on, the shader gamma is 1.0; see
         * font_text_gamma().
         *
         * The CONFIG default is auto (config.c, font_smoothing_level): off at 2x,
         * where smoothing plus linear-light compositing over-inks light-on-dark text,
         * and light at 1x (glyph_fill_grey), where an undilated stem is one partly lit
         * pixel column and reads stepped while the full dilation reads bold. Terminal glyphs go through the shader's blend
         * correction instead, which skips the coverage gamma, so the halo problem above
         * does not arise there; the gamma still applies to UI chrome, which has no
         * background. */
        CGContextSetAllowsFontSmoothing(g_glyph_ctx, g_opt_smoothing != FONT_SMOOTH_OFF);
        CGContextSetShouldSmoothFonts(g_glyph_ctx, g_opt_smoothing != FONT_SMOOTH_OFF);
        /* Subpixel positioning OFF: the glyph cache is keyed by codepoint
         * only, so a position-dependent raster would be cached at one x and
         * reused at every other x, smearing the result. Pixel-snapped glyphs
         * stay sharp and predictable for a monospace terminal grid. */
        CGContextSetAllowsFontSubpixelPositioning(g_glyph_ctx, false);
        CGContextSetShouldSubpixelPositionFonts(g_glyph_ctx, false);
        CGContextSetAllowsFontSubpixelQuantization(g_glyph_ctx, false);
        CGContextSetShouldSubpixelQuantizeFonts(g_glyph_ctx, false);
        CGContextScaleCTM(g_glyph_ctx, (CGFloat)FONT_OVERSAMPLE, (CGFloat)FONT_OVERSAMPLE);
        g_glyph_ctx_w = cw;
        g_glyph_ctx_h = ch;
    }
    return g_glyph_ctx;
}

/* 2×2 box-filter downsample of the OVERSAMPLE× alpha buffer into R8.
 * CG writes geometric coverage (linear), so a plain arithmetic mean is the
 * mathematically correct gamma-aware operation here. */
static void downsample_glyph_buf(u8 *dst, i32 cw, i32 ch) {
    i32 bw = cw * FONT_OVERSAMPLE;
#if FONT_OVERSAMPLE == 1
    /* No supersampling — the context already wrote target-resolution coverage. */
    memcpy(dst, g_glyph_buf, (usize)cw * (usize)ch);
    (void)bw;
#elif FONT_OVERSAMPLE == 2
    for (i32 y = 0; y < ch; y++) {
        const u8 *row0 = g_glyph_buf + (y * 2)     * bw;
        const u8 *row1 = g_glyph_buf + (y * 2 + 1) * bw;
        u8       *out  = dst + y * cw;
        for (i32 x = 0; x < cw; x++) {
            u32 s = row0[x*2 + 0] + row0[x*2 + 1] + row1[x*2 + 0] + row1[x*2 + 1];
            out[x] = (u8)((s + 2) >> 2);
        }
    }
#else
    for (i32 y = 0; y < ch; y++) {
        for (i32 x = 0; x < cw; x++) {
            u32 s = 0;
            for (i32 dy = 0; dy < FONT_OVERSAMPLE; dy++)
                for (i32 dx = 0; dx < FONT_OVERSAMPLE; dx++) {
                    s += g_glyph_buf[(y*FONT_OVERSAMPLE + dy) * bw + x*FONT_OVERSAMPLE + dx];
                }
            u32 d = FONT_OVERSAMPLE * FONT_OVERSAMPLE;
            dst[y * cw + x] = (u8)(s / d);
        }
    }
#endif
}

/* Rasterize a glyph using Core Text — pixel-perfect on macOS */
/* One-entry cache for a size-scaled copy of a CTFont.
 *
 * Large UI text (markdown headings) is rasterised at its real pixel size
 * instead of being magnified out of the atlas cell, which means asking Core
 * Text for the same face at N× the size. Every glyph of one heading needs the
 * same copy, and the raster runs in bursts of one size, so a single slot hits
 * essentially always. Callers hold g_ct_mu, which is what makes the static
 * state safe against the async raster worker. */
static CTFontRef s_scaled_font = NULL;
static CTFontRef s_scaled_base = NULL;   /* compared by pointer only, never dereferenced */
static f32       s_scaled_mul  = 0.0f;

static CTFontRef ct_scaled_font(CTFontRef base, f32 mul) {
    if (!base || mul <= 1.001f) return base;
    if (s_scaled_font && s_scaled_base == base && fabsf(s_scaled_mul - mul) < 0.001f)
        return s_scaled_font;
    CTFontRef copy = CTFontCreateCopyWithAttributes(
        base, CTFontGetSize(base) * (CGFloat)mul, NULL, NULL);
    if (!copy) return base;
    if (s_scaled_font) CFRelease(s_scaled_font);
    s_scaled_font = copy;
    s_scaled_base = base;
    s_scaled_mul  = mul;
    return s_scaled_font;
}

/* Shaping attributes for the ligature path, cached per CTFont for the same
 * reason the scaled copy is: the dictionary is font-invariant, and
 * rebuilding it on every cache miss would put CF churn on the shaping hot
 * path. Lives up here beside the other pointer-keyed cache so
 * ct_font_cache_forget() can drop both. */
static CFDictionaryRef s_lig_attrs      = NULL;
static CTFontRef       s_lig_attrs_font = NULL;   /* key only; never dereferenced */

/* Drop whichever of the two caches was keyed on `base`. Must be called before
 * the caller releases a font it owns: both keys are bare pointers, so a freed
 * address could later be matched by a *different* font allocated at the same
 * place and hand back the wrong face — a wrong-size raster from the scaled
 * slot, or shaping through the previous face from the attributes dictionary
 * (which holds its own retain on the font, so the stale object stays alive
 * and the failure is silent rather than a crash).
 *
 * Lock discipline: callers hold g_ct_mu, OR g_face_mu — every acquisition of
 * g_ct_mu in this file is nested inside g_face_mu (the raster entry points
 * take the face lock and then the context lock), so holding the outer one
 * excludes every reader of these two slots. The teardown sites below are all
 * on the g_face_mu side. */
/* The attribute dictionary is font-invariant, so it is built once per CTFont
 * and reused — rebuilding it on every cache miss added CF churn to the
 * shaping hot path. Rebuilt when the font changes (size/family reload swaps
 * g_ct_font, and ct_font_cache_forget drops this slot with it); main-thread
 * only, like every ligature call, and with g_face_mu held. */
static CFDictionaryRef ct_lig_attrs_locked(void) {
    if (!g_ct_font) return NULL;
    if (!s_lig_attrs || s_lig_attrs_font != g_ct_font) {
        if (s_lig_attrs) { CFRelease(s_lig_attrs); s_lig_attrs = NULL; }
        CFMutableDictionaryRef attrs = CFDictionaryCreateMutable(NULL, 2,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        if (!attrs) return NULL;
        CFDictionarySetValue(attrs, kCTFontAttributeName, g_ct_font);
        /* Request standard ligatures (value 1) */
        i32 lig_val = 1;
        CFNumberRef lig_num = CFNumberCreate(NULL, kCFNumberSInt32Type, &lig_val);
        CFDictionarySetValue(attrs, kCTLigatureAttributeName, lig_num);
        CFRelease(lig_num);
        s_lig_attrs = attrs;
        s_lig_attrs_font = g_ct_font;
    }
    return s_lig_attrs;
}

static void ct_font_cache_forget(CTFontRef base) {
    if (!base) return;
    if (s_scaled_base == base) {
        if (s_scaled_font) CFRelease(s_scaled_font);
        s_scaled_font = NULL;
        s_scaled_base = NULL;
        s_scaled_mul  = 0.0f;
    }
    if (s_lig_attrs_font == base) {
        if (s_lig_attrs) CFRelease(s_lig_attrs);
        s_lig_attrs = NULL;
        s_lig_attrs_font = NULL;
    }
}

static u8 *mmap_font_file(const char *path, usize *out_size);
static void unmap_font_file(u8 *data, usize size);
static bool font_probe(const u8 *data, usize size, u32 *offset);
static CTFontRef ct_font_from_file(const char *path, f32 pixel_h);
static CTFontRef ct_font_apply_options(CTFontRef f, bool variations);

/* The language a fallback is chosen for: the user's first preferred language,
 * so a Han codepoint is drawn in the forms of that language (Japanese, Simplified
 * or Traditional Chinese, Korean) rather than whichever face the cascade lists
 * first. NULL lets Core Text use the system language. Read once. */
static CFStringRef ct_fallback_language(void) {
    static CFStringRef lang = NULL;
    static bool looked = false;
    if (looked) return lang;
    looked = true;
    CFArrayRef prefs = CFLocaleCopyPreferredLanguages();
    if (prefs) {
        if (CFArrayGetCount(prefs) > 0) {
            CFStringRef first = (CFStringRef)CFArrayGetValueAtIndex(prefs, 0);
            if (first) lang = CFStringCreateCopy(NULL, first);
        }
        CFRelease(prefs);
    }
    return lang;
}

static bool ct_font_has_cp(CTFontRef f, u32 cp) {
    if (!f) return false;
    UniChar u[2];
    CGGlyph g[2] = {0, 0};
    CFIndex n = 1;
    if (cp > 0xFFFF) {
        u[0] = (UniChar)(((cp - 0x10000) >> 10) + 0xD800);
        u[1] = (UniChar)(((cp - 0x10000) & 0x3FF) + 0xDC00);
        n = 2;
    } else {
        u[0] = (UniChar)cp;
    }
    return CTFontGetGlyphsForCharacters(f, u, g, n) && g[0] != 0;
}

/* A face Core Text found for a codepoint the chain did not cover, resized so
 * its lowercase stands as tall as the primary's: faces draw the same point
 * size at different visual sizes, and a symbol or a script from a face with a
 * small x-height would read as a smaller font. A face with ideographs keeps
 * its size; the two-cell fit in ct_rasterize_glyph_m sizes those. Takes
 * ownership of `fb` and returns the font to keep. */
static CTFontRef ct_fallback_sized(CTFontRef fb) {
    if (!fb || !g_ct_font || ct_font_has_cp(fb, 0x6C34)) return fb;
    CGFloat psz = CTFontGetSize(g_ct_font), fsz = CTFontGetSize(fb);
    if (psz <= 0 || fsz <= 0) return fb;
    CGFloat pxh = CTFontGetXHeight(g_ct_font) / psz;
    CGFloat fxh = CTFontGetXHeight(fb) / fsz;
    /* A face that does not state a plausible x-height is compared on caps. */
    if (!(pxh > 0.25 && pxh < 0.9) || !(fxh > 0.25 && fxh < 0.9)) {
        pxh = CTFontGetCapHeight(g_ct_font) / psz;
        fxh = CTFontGetCapHeight(fb) / fsz;
        if (!(pxh > 0.35 && pxh < 1.2) || !(fxh > 0.35 && fxh < 1.2)) return fb;
    }
    CGFloat s = CLAMP(pxh / fxh, 0.8, 1.25);
    if (fabs(s - 1.0) < 0.02) return fb;
    CTFontRef sized = CTFontCreateCopyWithAttributes(fb, fsz * s, NULL, NULL);
    if (!sized) return fb;
    CFRelease(fb);
    return sized;
}

/* The weight axis of a variable face: its default, minimum and maximum.
 * False when the face has none. */
static bool ct_font_wght_axis(CTFontRef f, f64 *def, f64 *mn, f64 *mx) {
    CFArrayRef axes = f ? CTFontCopyVariationAxes(f) : NULL;
    if (!axes) return false;
    bool found = false;
    for (CFIndex i = 0; i < CFArrayGetCount(axes) && !found; i++) {
        CFDictionaryRef a = (CFDictionaryRef)CFArrayGetValueAtIndex(axes, i);
        CFNumberRef id = a ? CFDictionaryGetValue(a, kCTFontVariationAxisIdentifierKey) : NULL;
        i64 tag = 0;
        if (!id || !CFNumberGetValue(id, kCFNumberSInt64Type, &tag) || tag != 0x77676874) continue;
        CFNumberRef d = CFDictionaryGetValue(a, kCTFontVariationAxisDefaultValueKey);
        CFNumberRef lo = CFDictionaryGetValue(a, kCTFontVariationAxisMinimumValueKey);
        CFNumberRef hi = CFDictionaryGetValue(a, kCTFontVariationAxisMaximumValueKey);
        found = d && lo && hi &&
                CFNumberGetValue(d, kCFNumberFloat64Type, def) &&
                CFNumberGetValue(lo, kCFNumberFloat64Type, mn) &&
                CFNumberGetValue(hi, kCFNumberFloat64Type, mx);
    }
    CFRelease(axes);
    return found;
}

/* A copy of `f` at weight `wght` on its weight axis, or NULL. */
static CTFontRef ct_font_at_weight(CTFontRef f, f64 wght) {
    i64 tag = 0x77676874;
    CFNumberRef k = CFNumberCreate(NULL, kCFNumberSInt64Type, &tag);
    CFNumberRef v = CFNumberCreate(NULL, kCFNumberFloat64Type, &wght);
    CTFontRef out = NULL;
    if (k && v) {
        const void *vk[1] = {k}, *vv[1] = {v};
        CFDictionaryRef var = CFDictionaryCreate(NULL, vk, vv, 1, &kCFTypeDictionaryKeyCallBacks,
                                                 &kCFTypeDictionaryValueCallBacks);
        if (var) {
            const void *ak[1] = {kCTFontVariationAttribute}, *av[1] = {var};
            CFDictionaryRef attrs = CFDictionaryCreate(NULL, ak, av, 1, &kCFTypeDictionaryKeyCallBacks,
                                                       &kCFTypeDictionaryValueCallBacks);
            if (attrs) {
                CTFontDescriptorRef d = CTFontDescriptorCreateWithAttributes(attrs);
                if (d) {
                    out = CTFontCreateCopyWithAttributes(f, 0.0, NULL, d);
                    CFRelease(d);
                }
                CFRelease(attrs);
            }
            CFRelease(var);
        }
    }
    if (k) CFRelease(k);
    if (v) CFRelease(v);
    return out;
}

/* The path of a style sibling of `path`: the word "Regular" in the file name
 * replaced by `word` ("Bold", "Italic", "BoldItalic"), or `-word` inserted
 * before the extension when the name has no "Regular". */
static bool face_sibling_path(const char *path, const char *word, char *out, usize n) {
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    const char *reg = strstr(base, "Regular");
    if (reg) {
        snprintf(out, n, "%.*s%s%s", (int)(reg - path), path, word, reg + 7);
        return true;
    }
    const char *dot = strrchr(base, '.');
    if (!dot) return false;
    snprintf(out, n, "%.*s-%s%s", (int)(dot - path), path, word, dot);
    return true;
}

/* A sibling face file, opened at the primary's size, if it exists and passes
 * the structural gate every face file passes. */
static CTFontRef ct_sibling_face(const char *word) {
    char sib[1024];
    if (!g_ct_font || !g_face.path[0] ||
        !face_sibling_path(g_face.path, word, sib, sizeof sib) ||
        strcmp(sib, g_face.path) == 0) return NULL;
    usize size = 0;
    u8 *data = mmap_font_file(sib, &size);
    if (!data) return NULL;
    u32 off = 0;
    bool ok = font_probe(data, size, &off);
    unmap_font_file(data, size);
    return ok ? ct_font_from_file(sib, (f32)CTFontGetSize(g_ct_font)) : NULL;
}

/* A face of the primary's family in the font file at `path` whose bold and
 * italic traits are exactly `bold` and `italic`, at the weight nearest Bold
 * (or Regular). A collection keeps every style in one file (Menlo.ttc), and a
 * family can keep its italics in one file of named instances, the first of
 * which is Light (SFNSMonoItalic.ttf). NULL when the file holds none. */
static CTFontRef ct_family_face_in_file(const char *path, bool bold, bool italic) {
    CFStringRef path_str = CFStringCreateWithCString(NULL, path, kCFStringEncodingUTF8);
    CFURLRef url = path_str ? CFURLCreateWithFileSystemPath(NULL, path_str, kCFURLPOSIXPathStyle, false) : NULL;
    if (path_str) CFRelease(path_str);
    CFArrayRef descs = url ? CTFontManagerCreateFontDescriptorsFromURL(url) : NULL;
    if (url) CFRelease(url);
    if (!descs) return NULL;
    CFStringRef family = CTFontCopyFamilyName(g_ct_font);
    CFStringRef own = CTFontCopyPostScriptName(g_ct_font);
    /* kCTFontWeightTrait is -1..1: Regular 0, Bold 0.4 (NSFontWeightBold). */
    CGFloat want = bold ? 0.4 : 0.0;
    CTFontRef best = NULL;
    CGFloat best_d = 0;
    for (CFIndex i = 0; family && own && i < CFArrayGetCount(descs); i++) {
        CTFontRef f = CTFontCreateWithFontDescriptor(
            (CTFontDescriptorRef)CFArrayGetValueAtIndex(descs, i), CTFontGetSize(g_ct_font), NULL);
        if (!f) continue;
        CTFontSymbolicTraits t = CTFontGetSymbolicTraits(f);
        CFStringRef fam = CTFontCopyFamilyName(f);
        CFStringRef ps = CTFontCopyPostScriptName(f);
        bool fits = ((t & kCTFontTraitBold) != 0) == bold && ((t & kCTFontTraitItalic) != 0) == italic &&
                    fam && CFStringCompare(fam, family, 0) == kCFCompareEqualTo &&
                    ps && CFStringCompare(ps, own, 0) != kCFCompareEqualTo;
        CGFloat w = 0;
        CFDictionaryRef traits = fits ? CTFontCopyTraits(f) : NULL;
        if (traits) {
            CFNumberRef n = CFDictionaryGetValue(traits, kCTFontWeightTrait);
            if (n) CFNumberGetValue(n, kCFNumberCGFloatType, &w);
            CFRelease(traits);
        }
        CGFloat d = fabs(w - want);
        if (fits && (!best || d < best_d)) {
            if (best) CFRelease(best);
            best = f;
            best_d = d;
        } else {
            CFRelease(f);
        }
        if (fam) CFRelease(fam);
        if (ps) CFRelease(ps);
    }
    if (family) CFRelease(family);
    if (own) CFRelease(own);
    CFRelease(descs);
    return best;
}

/* The family's own styled face beside the primary: in the primary's file, or
 * in the file named after the style with no dash (SFNSMono.ttf ->
 * SFNSMonoItalic.ttf), which passes the same structural gate as any face. A
 * bold italic is also looked for in the italic file. */
static CTFontRef ct_family_face_beside(bool bold, bool italic) {
    if (!g_ct_font || !g_face.path[0]) return NULL;
    CTFontRef f = ct_family_face_in_file(g_face.path, bold, italic);
    if (f) return f;
    const char *base = strrchr(g_face.path, '/');
    base = base ? base + 1 : g_face.path;
    const char *dot = strrchr(base, '.');
    if (!dot || strstr(base, "Regular")) return NULL;
    const char *words[2] = { bold && italic ? "BoldItalic" : bold ? "Bold" : "Italic",
                             bold && italic ? "Italic" : NULL };
    for (u32 k = 0; k < 2 && words[k] && !f; k++) {
        char sib[1024];
        snprintf(sib, sizeof sib, "%.*s%s%s", (int)(dot - g_face.path), g_face.path, words[k], dot);
        usize size = 0;
        u8 *data = mmap_font_file(sib, &size);
        if (!data) continue;
        u32 off = 0;
        bool ok = font_probe(data, size, &off);
        unmap_font_file(data, size);
        if (ok) f = ct_family_face_in_file(sib, bold, italic);
    }
    return f;
}

/* The face a styled glyph is drawn from, and what has to be synthesised on top
 * of it. `style` holds FONT_STYLE_* bits; 0 is the regular face. Called with
 * g_face_mu held. */
static CTFontRef ct_styled_face_locked(u32 style, u8 *synth) {
    u32 i = (style & FONT_STYLE_MASK) >> 28;
    *synth = 0;
    if (!i || !g_ct_font) return g_ct_font;
    if (!g_ct_styled_tried[i]) {
        g_ct_styled_tried[i] = true;
        bool bold = (i & 1u) != 0, italic = (i & 2u) != 0;
        CTFontRef f = ct_sibling_face(bold && italic ? "BoldItalic" : bold ? "Bold" : "Italic");
        u8 s = 0;
        if (!f) f = ct_family_face_beside(bold, italic);
        if (!f && bold) {
            /* A variable face carries its own bold on the weight axis. */
            f64 def = 0, mn = 0, mx = 0;
            if (ct_font_wght_axis(g_ct_font, &def, &mn, &mx) && mx >= 600.0)
                f = ct_font_at_weight(g_ct_font, mx < 700.0 ? mx : 700.0);
            if (f && italic) s |= STYLED_SYNTH_ITALIC;
        }
        if (!f && bold && italic) {
            f = ct_sibling_face("Bold");
            if (!f) f = ct_family_face_beside(true, false);
            if (f) s |= STYLED_SYNTH_ITALIC;
            else if ((f = ct_sibling_face("Italic")) != NULL ||
                     (f = ct_family_face_beside(false, true)) != NULL) s |= STYLED_SYNTH_BOLD;
        }
        if (!f) {
            f = (CTFontRef)CFRetain(g_ct_font);
            s = (u8)((bold ? STYLED_SYNTH_BOLD : 0) | (italic ? STYLED_SYNTH_ITALIC : 0));
        }
        if (f != g_ct_font) f = ct_font_apply_options(f, false);
        g_ct_styled[i] = f;
        g_ct_styled_synth[i] = s;
    }
    if (!g_ct_styled[i]) return g_ct_font;
    *synth = g_ct_styled_synth[i];
    return g_ct_styled[i];
}

/* Drop the styled faces; the next styled glyph rebuilds them against the
 * face that is loaded then. g_face_mu held. */
static void ct_cpmap_release(void);
static void ct_styled_faces_release(void) {
    ct_cpmap_release();
    for (u32 i = 0; i < 4; i++) {
        if (g_ct_styled[i]) {
            ct_font_cache_forget(g_ct_styled[i]);
            CFRelease(g_ct_styled[i]);
        }
        g_ct_styled[i] = NULL;
        g_ct_styled_synth[i] = 0;
        g_ct_styled_tried[i] = false;
    }
}

/* The face that draws `cp` when the primary does not: the open chain first,
 * then one queued candidate at a time, then Core Text's own choice in the
 * user's language, sized to the primary's lowercase and added to the chain.
 * Fills `glyphs`. Returns NULL when nothing on the machine has it. A face the
 * full chain could not adopt is returned through `owned` as well, and the
 * caller releases it after its last use. Takes g_fb_mu, so the caller holds
 * at most g_face_mu and g_ct_mu. */
static CTFontRef ct_chain_font_for(u32 cp, const UniChar *uchars, i32 ulen,
                                   CGGlyph *glyphs, CTFontRef *owned) {
    CTFontRef font = NULL;
    *owned = NULL;
    pthread_mutex_lock(&g_fb_mu);
    i32 scanned = 0;
    do {
        for (; scanned < g_ct_fallback_count; scanned++) {
            if (g_ct_fallbacks[scanned] &&
                CTFontGetGlyphsForCharacters(g_ct_fallbacks[scanned], uchars, glyphs, ulen)) {
                font = g_ct_fallbacks[scanned];
                break;
            }
        }
    } while (!font && load_next_lazy_fallback(cp));
    if (!font) {
        CFStringRef str = CFStringCreateWithCharacters(NULL, uchars, ulen);
        CTFontRef fallback = str ? CTFontCreateForStringWithLanguage(
            g_ct_font, str, CFRangeMake(0, ulen), ct_fallback_language()) : NULL;
        if (str) CFRelease(str);
        if (fallback && CTFontGetGlyphsForCharacters(fallback, uchars, glyphs, ulen)) {
            fallback = ct_fallback_sized(fallback);
            if (CTFontGetGlyphsForCharacters(fallback, uchars, glyphs, ulen)) {
                font = fallback;
                /* Kept in the chain for the next codepoint it covers. When the
                 * chain is full it is still used for this raster, owned by the
                 * caller: evicting an existing entry is not safe, because the
                 * scan above hands chain fonts to other threads that keep using
                 * them after g_fb_mu is dropped. */
                if (g_ct_fallback_count < MAX_FALLBACK_FONTS)
                    g_ct_fallbacks[g_ct_fallback_count++] = fallback;
                else
                    *owned = fallback;
            }
        }
        if (fallback && fallback != font) CFRelease(fallback);
    }
    pthread_mutex_unlock(&g_fb_mu);
    return font;
}

/* ---- Face options ---------------------------------------------------------
 * font_features, font_variations and font_codepoint_map from the config,
 * applied when the face loads (face_load_locked). */

/* A four-letter OpenType tag at `s`, as the axis identifier Core Text takes. */
static bool ct_tag_at(const char *s, usize n, i64 *tag) {
    if (n != 4) return false;
    *tag = ((i64)(u8)s[0] << 24) | ((i64)(u8)s[1] << 16) | ((i64)(u8)s[2] << 8) | (u8)s[3];
    return true;
}

/* Split `list` at commas, semicolons or blanks and hand each item to `fn`. */
static void opt_items(const char *list, void (*fn)(const char *item, usize n, void *ctx), void *ctx) {
    const char *p = list;
    while (p && *p) {
        while (*p == ',' || *p == ';' || *p == ' ' || *p == '\t') p++;
        const char *q = p;
        while (*q && *q != ',' && *q != ';' && *q != ' ' && *q != '\t') q++;
        if (q > p) fn(p, (usize)(q - p), ctx);
        p = q;
    }
}

/* "zero", "+zero", "-calt", "cv01=2" -> {tag, value} into a CFArray. */
static void opt_feature(const char *item, usize n, void *ctx) {
    CFMutableArrayRef arr = (CFMutableArrayRef)ctx;
    i32 value = 1;
    if (*item == '+' || *item == '-') { value = *item == '-' ? 0 : 1; item++; n--; }
    const char *eq = memchr(item, '=', n);
    usize tn = eq ? (usize)(eq - item) : n;
    if (eq) value = atoi(eq + 1);
    if (tn != 4) return;
    char tag[5] = {item[0], item[1], item[2], item[3], 0};
    CFStringRef t = CFStringCreateWithCString(NULL, tag, kCFStringEncodingASCII);
    CFNumberRef v = CFNumberCreate(NULL, kCFNumberSInt32Type, &value);
    if (t && v) {
        const void *k[2] = {kCTFontOpenTypeFeatureTag, kCTFontOpenTypeFeatureValue};
        const void *vv[2] = {t, v};
        CFDictionaryRef d = CFDictionaryCreate(NULL, k, vv, 2, &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
        if (d) { CFArrayAppendValue(arr, d); CFRelease(d); }
    }
    if (t) CFRelease(t);
    if (v) CFRelease(v);
}

/* "wght=450" -> {axis: value} into a CFDictionary. */
static void opt_variation(const char *item, usize n, void *ctx) {
    CFMutableDictionaryRef dict = (CFMutableDictionaryRef)ctx;
    const char *eq = memchr(item, '=', n);
    i64 tag = 0;
    if (!eq || !ct_tag_at(item, (usize)(eq - item), &tag)) return;
    char num[32];
    usize vn = n - (usize)(eq + 1 - item);
    if (vn == 0 || vn >= sizeof num) return;
    memcpy(num, eq + 1, vn);
    num[vn] = 0;
    f64 value = atof(num);
    CFNumberRef k = CFNumberCreate(NULL, kCFNumberSInt64Type, &tag);
    CFNumberRef v = CFNumberCreate(NULL, kCFNumberFloat64Type, &value);
    if (k && v) CFDictionarySetValue(dict, k, v);
    if (k) CFRelease(k);
    if (v) CFRelease(v);
}

/* `f` with the configured features and variation axes, or `f` itself when
 * there are none or the face refuses them. Takes ownership of `f`. */
static CTFontRef ct_font_apply_options(CTFontRef f, bool variations) {
    if (!f || (!g_opt_features[0] && (!variations || !g_opt_variations[0]))) return f;
    CFMutableDictionaryRef attrs = CFDictionaryCreateMutable(NULL, 2, &kCFTypeDictionaryKeyCallBacks,
                                                             &kCFTypeDictionaryValueCallBacks);
    if (!attrs) return f;
    if (g_opt_features[0]) {
        CFMutableArrayRef arr = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
        if (arr) {
            opt_items(g_opt_features, opt_feature, arr);
            if (CFArrayGetCount(arr) > 0) CFDictionarySetValue(attrs, kCTFontFeatureSettingsAttribute, arr);
            CFRelease(arr);
        }
    }
    if (variations && g_opt_variations[0]) {
        CFMutableDictionaryRef var = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
                                                               &kCFTypeDictionaryValueCallBacks);
        if (var) {
            opt_items(g_opt_variations, opt_variation, var);
            if (CFDictionaryGetCount(var) > 0) CFDictionarySetValue(attrs, kCTFontVariationAttribute, var);
            CFRelease(var);
        }
    }
    CTFontRef out = f;
    if (CFDictionaryGetCount(attrs) > 0) {
        CTFontDescriptorRef d = CTFontDescriptorCreateWithAttributes(attrs);
        if (d) {
            CTFontRef copy = CTFontCreateCopyWithAttributes(f, 0.0, NULL, d);
            if (copy) { CFRelease(f); out = copy; }
            CFRelease(d);
        }
    }
    CFRelease(attrs);
    return out;
}

/* The glyph the configured features choose for one character of `font` —
 * Core Text applies substitution features (zero, ss01, cv01) only when it
 * shapes, and a single glyph is otherwise looked up straight from the cmap. */
static CGGlyph ct_feature_glyph(CTFontRef font, const UniChar *u, i32 n, CGGlyph plain) {
    if (!g_opt_features[0]) return plain;
    CFStringRef str = CFStringCreateWithCharacters(NULL, u, n);
    if (!str) return plain;
    const void *k[1] = {kCTFontAttributeName}, *v[1] = {font};
    CFDictionaryRef attrs = CFDictionaryCreate(NULL, k, v, 1, &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    CFAttributedStringRef astr = attrs ? CFAttributedStringCreate(NULL, str, attrs) : NULL;
    CTLineRef line = astr ? CTLineCreateWithAttributedString(astr) : NULL;
    CGGlyph out = plain;
    if (line) {
        CFArrayRef runs = CTLineGetGlyphRuns(line);
        if (runs && CFArrayGetCount(runs) == 1) {
            CTRunRef run = (CTRunRef)CFArrayGetValueAtIndex(runs, 0);
            CTFontRef rf = (CTFontRef)CFDictionaryGetValue(CTRunGetAttributes(run), kCTFontAttributeName);
            CGGlyph g = 0;
            if (CTRunGetGlyphCount(run) == 1 && rf && CFEqual(rf, font)) {
                CTRunGetGlyphs(run, CFRangeMake(0, 1), &g);
                if (g) out = g;
            }
        }
        CFRelease(line);
    }
    if (astr) CFRelease(astr);
    if (attrs) CFRelease(attrs);
    CFRelease(str);
    return out;
}

/* The codepoint map: ranges drawn from a named face before the primary. */
#define CPMAP_MAX 16
static struct { u32 lo, hi; CTFontRef font; } g_cpmap[CPMAP_MAX];
static i32 g_cpmap_count = 0;

static void ct_cpmap_release(void) {
    for (i32 i = 0; i < g_cpmap_count; i++) {
        if (g_cpmap[i].font) {
            ct_font_cache_forget(g_cpmap[i].font);
            CFRelease(g_cpmap[i].font);
        }
        g_cpmap[i].font = NULL;
    }
    g_cpmap_count = 0;
}

static bool font_path_for_name(const char *name, char *out, usize n);

/* A face named by the user: a path when it has a '/', else a family or
 * PostScript name. NULL when nothing installed has that name — Core Text would
 * otherwise hand back some other face for an unknown name. */
static CTFontRef ct_font_named(const char *name, f32 size) {
    char path[1024];
    if (strchr(name, '/')) snprintf(path, sizeof path, "%s", name);
    else if (!font_path_for_name(name, path, sizeof path)) return NULL;
    usize sz = 0;
    u8 *data = mmap_font_file(path, &sz);
    if (!data) return NULL;
    u32 off = 0;
    bool ok = font_probe(data, sz, &off);
    unmap_font_file(data, sz);
    return ok ? ct_font_from_file(path, size) : NULL;
}

/* "U+E000-U+F8FF=Symbols Nerd Font; U+2190-U+21FF=/path/face.ttf". */
static void ct_cpmap_load_locked(f32 size) {
    ct_cpmap_release();
    const char *p = g_opt_cpmap;
    while (*p && g_cpmap_count < CPMAP_MAX) {
        while (*p == ';' || *p == ' ' || *p == '\t' || *p == ',') p++;
        const char *end = strchr(p, ';');
        if (!end) end = p + strlen(p);
        const char *eq = memchr(p, '=', (usize)(end - p));
        if (eq) {
            const char *r = p;
            if ((r[0] == 'U' || r[0] == 'u') && r[1] == '+') r += 2;
            char *e = NULL;
            unsigned long lo = strtoul(r, &e, 16), hi = lo;
            if (e && *e == '-') {
                r = e + 1;
                if ((r[0] == 'U' || r[0] == 'u') && r[1] == '+') r += 2;
                hi = strtoul(r, &e, 16);
            }
            char name[512];
            const char *nb = eq + 1;
            while (nb < end && (*nb == ' ' || *nb == '\t')) nb++;
            usize nl = (usize)(end - nb);
            while (nl > 0 && (nb[nl - 1] == ' ' || nb[nl - 1] == '\t')) nl--;
            if (e && e <= eq && lo <= hi && hi <= 0x10FFFF && nl > 0 && nl < sizeof name) {
                memcpy(name, nb, nl);
                name[nl] = 0;
                CTFontRef f = ct_font_named(name, size);
                if (f) {
                    g_cpmap[g_cpmap_count].lo = (u32)lo;
                    g_cpmap[g_cpmap_count].hi = (u32)hi;
                    g_cpmap[g_cpmap_count].font = f;
                    g_cpmap_count++;
                } else {
                    fprintf(stderr, "font: codepoint map: no face named '%s'\n", name);
                }
            }
        }
        p = *end ? end + 1 : end;
    }
}

static CTFontRef ct_cpmap_font(u32 cp) {
    for (i32 i = 0; i < g_cpmap_count; i++)
        if (cp >= g_cpmap[i].lo && cp <= g_cpmap[i].hi) return g_cpmap[i].font;
    return NULL;
}

/* The file of the installed face called `name` — a family name (its Regular)
 * or a PostScript name — when it can be loaded by path: a single-face file, or
 * the first face of a collection. */
static bool font_path_for_name(const char *name, char *out, usize n) {
    if (!name || !*name) return false;
    CFStringRef s = CFStringCreateWithCString(NULL, name, kCFStringEncodingUTF8);
    if (!s) return false;
    bool found = false;
    CFStringRef keys[2] = {kCTFontFamilyNameAttribute, kCTFontNameAttribute};
    for (i32 k = 0; k < 2 && !found; k++) {
        const void *ak[1] = {keys[k]}, *av[1] = {s};
        CFDictionaryRef attrs = CFDictionaryCreate(NULL, ak, av, 1, &kCFTypeDictionaryKeyCallBacks,
                                                   &kCFTypeDictionaryValueCallBacks);
        CTFontDescriptorRef d = attrs ? CTFontDescriptorCreateWithAttributes(attrs) : NULL;
        CFSetRef mandatory = CFSetCreate(NULL, (const void **)&keys[k], 1, &kCFTypeSetCallBacks);
        CFArrayRef matches = (d && mandatory) ? CTFontDescriptorCreateMatchingFontDescriptors(d, mandatory) : NULL;
        f64 best = 1e9;
        for (CFIndex i = 0; matches && i < CFArrayGetCount(matches); i++) {
            CTFontDescriptorRef m = (CTFontDescriptorRef)CFArrayGetValueAtIndex(matches, i);
            CFURLRef url = CTFontDescriptorCopyAttribute(m, kCTFontURLAttribute);
            CFDictionaryRef traits = CTFontDescriptorCopyAttribute(m, kCTFontTraitsAttribute);
            CFStringRef ps = CTFontDescriptorCopyAttribute(m, kCTFontNameAttribute);
            f64 w = 0.0;
            u32 sym = 0;
            if (traits) {
                CFNumberRef wn = CFDictionaryGetValue(traits, kCTFontWeightTrait);
                CFNumberRef sn = CFDictionaryGetValue(traits, kCTFontSymbolicTrait);
                if (wn) CFNumberGetValue(wn, kCFNumberFloat64Type, &w);
                if (sn) CFNumberGetValue(sn, kCFNumberSInt32Type, &sym);
            }
            /* Regular first: upright, weight closest to 0. */
            f64 score = fabs(w) + ((sym & kCTFontTraitItalic) ? 10.0 : 0.0) +
                        ((sym & kCTFontTraitBold) ? 5.0 : 0.0);
            char path[1024];
            if (url && ps && score < best &&
                CFURLGetFileSystemRepresentation(url, true, (UInt8 *)path, sizeof path)) {
                /* A collection loads its first face; take it only when that is
                 * this one. */
                bool first = true;
                const char *dot = strrchr(path, '.');
                if (dot && (strcasecmp(dot, ".ttc") == 0 || strcasecmp(dot, ".otc") == 0)) {
                    CTFontRef f0 = ct_font_from_file(path, 12.0f);
                    CFStringRef ps0 = f0 ? CTFontCopyPostScriptName(f0) : NULL;
                    first = ps0 && CFEqual(ps0, ps);
                    if (ps0) CFRelease(ps0);
                    if (f0) CFRelease(f0);
                }
                if (first) {
                    best = score;
                    snprintf(out, n, "%s", path);
                    found = true;
                }
            }
            if (url) CFRelease(url);
            if (traits) CFRelease(traits);
            if (ps) CFRelease(ps);
        }
        if (matches) CFRelease(matches);
        if (mandatory) CFRelease(mandatory);
        if (d) CFRelease(d);
        if (attrs) CFRelease(attrs);
    }
    CFRelease(s);
    return found;
}


/* How a glyph is fitted into its box.
 *
 * TEXT keeps the face's own placement, and is only scaled down when its ink is
 * wider than the box (a face's ǅ, a long arrow, a glyph from a wide fallback):
 * the box is all the atlas holds, so anything outside it would be cut off.
 * SYMBOL is TEXT that is also brought inside vertically. ICON is a glyph of a
 * symbols font's private use area: sized to the icon height or the width it
 * may use, aspect kept, and centred on the middle of the capitals. STRETCH
 * fills the box on both axes — the prompt segment shapes that have to meet the
 * cells beside them. */
enum { GLYPH_FIT_TEXT, GLYPH_FIT_SYMBOL, GLYPH_FIT_ICON, GLYPH_FIT_STRETCH };

static u8 glyph_fit_kind(u32 cp) {
    if (cp >= 0xE0C0 && cp <= 0xE0D7) return GLYPH_FIT_STRETCH;
    if ((cp >= 0xE000 && cp <= 0xF8FF) || (cp >= 0xF0000 && cp <= 0x10FFFD))
        return GLYPH_FIT_ICON;
    /* Bracket, integral and summation pieces stack across rows: they keep
     * the face's own placement. */
    if ((cp >= 0x2320 && cp <= 0x2321) || (cp >= 0x239B && cp <= 0x23B3) || cp == 0x23B7)
        return GLYPH_FIT_TEXT;
    if ((cp >= 0x2000 && cp <= 0x2BFF) || (cp >= 0x1F000 && cp <= 0x1FBFF))
        return GLYPH_FIT_SYMBOL;
    return GLYPH_FIT_TEXT;
}

/* Where a glyph's ink goes in its box: the transform from glyph space (origin
 * on the baseline) into the context, as a scale and an offset. `ink` is the
 * glyph's bounding box at the size it is drawn, `pen` and `base` the face's own
 * placement of its origin. The context is y-up, 0 at the bottom of the box. */
typedef struct { CGFloat sx, sy, tx, ty; } GlyphPlace;

/* The whole-pixel move nearest `shift` that leaves the least of the ink span
 * [lo, hi] outside [0, box]. A glyph moved by whole pixels keeps the pixel
 * phase its face drew it at, so it is as sharp as the unmoved glyphs beside
 * it; a fractional move greys every vertical stem on a 1x display. */
static CGFloat whole_px_shift(CGFloat shift, CGFloat lo, CGFloat hi, CGFloat box) {
    CGFloat best = round(shift), best_out = INFINITY;
    for (CGFloat k = floor(shift); k <= ceil(shift); k += 1.0) {
        CGFloat out = MAX(0.0, -(lo + k)) + MAX(0.0, hi + k - box);
        if (out < best_out - 1e-6) { best_out = out; best = k; }
    }
    return best;
}

/* `icon_w` is the width an ICON may use, from the box's left edge: the whole
 * box for one cell, less a gap when the icon has taken the blank after it. */
static GlyphPlace glyph_place(u8 kind, CGRect ink, CGFloat pen, CGFloat base,
                              CGFloat box_w, CGFloat box_h, CGFloat icon_w, CGFloat icon_h,
                              CGFloat icon_mid, CGFloat stroke) {
    GlyphPlace p = {1.0, 1.0, pen, base};
    if (CGRectIsNull(ink) || ink.size.width <= 0.0 || ink.size.height <= 0.0) return p;
    /* The stroke a weight or a synthesised bold adds counts for the shapes
     * fitted to the box, not for text: a bold W fitted with its stroke would be
     * drawn smaller than the bold letters beside it. */
    if (kind < GLYPH_FIT_ICON) stroke = 0.0;
    CGFloat iw = ink.size.width + stroke, ih = ink.size.height + stroke;
    CGFloat ix = ink.origin.x - stroke * 0.5, iy = ink.origin.y - stroke * 0.5;
    if (kind == GLYPH_FIT_STRETCH) {
        p.sx = box_w / iw;
        p.sy = box_h / ih;
        p.tx = -ix * p.sx;
        p.ty = -iy * p.sy;
        return p;
    }
    if (kind == GLYPH_FIT_ICON) {
        CGFloat s = MIN(icon_w / iw, icon_h / ih);
        p.sx = p.sy = s;
        p.tx = (icon_w - iw * s) * 0.5 - ix * s;
        p.ty = icon_mid - ih * s * 0.5 - iy * s;
        if (p.ty < -iy * s) p.ty = -iy * s;                         /* not below the box */
        if (p.ty + (iy + ih) * s > box_h) p.ty = box_h - (iy + ih) * s;
        return p;
    }
    CGFloat left = pen + ix, right = left + iw;
    if (iw > box_w + 1.0) {
        CGFloat s = box_w / iw;
        p.sx = p.sy = s;
        p.tx = (box_w - iw * s) * 0.5 - ix * s;
        p.ty = base;
    } else if (left < -0.5 || right > box_w + 0.5) {
        /* Fits, but its own placement hangs it over an edge: move it in. */
        CGFloat shift = left < 0.0 ? -left : box_w - right;
        p.tx = pen + whole_px_shift(shift, left, right, box_w);
    }
    if (kind == GLYPH_FIT_SYMBOL) {
        CGFloat bottom = p.ty + iy * p.sy, top = bottom + ih * p.sy;
        if (top - bottom > box_h) {
            CGFloat s = box_h / ih;
            CGFloat cx = p.tx + (ix + iw * 0.5) * p.sx;              /* keep its centre */
            p.sx = p.sy = s;
            p.tx = cx - (ix + iw * 0.5) * s;
            p.ty = -iy * s;
        } else if (bottom < 0.0 || top > box_h) {
            CGFloat shift = bottom < 0.0 ? -bottom : box_h - top;
            p.ty += p.sy == 1.0 ? whole_px_shift(shift, bottom, top, box_h) : shift;
        }
    }
    return p;
}

/* A synthesised slant leans the ink out of its box: the bottom left, the top
 * right. Re-centre the slanted ink in the box, so a glyph loses as little as
 * it can on either side and a stroke that spans the cell (an underscore)
 * still meets the next one. `ink` is the glyph's box in glyph space, `p` its
 * placement; the slant pivots on the middle of the box (glyph_place_apply). */
static void glyph_place_center_slant(GlyphPlace *p, CGRect ink, CGFloat box_w, CGFloat box_h) {
    if (CGRectIsNull(ink) || ink.size.width <= 0.0 || ink.size.height <= 0.0) return;
    CGFloat k = (CGFloat)SYNTH_ITALIC_SHEAR;
    CGFloat x0 = p->tx + ink.origin.x * p->sx, x1 = x0 + ink.size.width * p->sx;
    CGFloat y0 = p->ty + ink.origin.y * p->sy, y1 = y0 + ink.size.height * p->sy;
    CGFloat lo = x0 + k * (y0 - box_h * 0.5), hi = x1 + k * (y1 - box_h * 0.5);
    p->tx += box_w * 0.5 - (lo + hi) * 0.5;
}

/* The same, measured on the glyph's own outline rather than its box: a glyph
 * whose widest part is at its foot (i, k, R) leans less far right than its box
 * does, and centring the box would push that foot out on the left. */
static void glyph_place_center_slant_path(GlyphPlace *p, CTFontRef font, CGGlyph glyph,
                                          CGRect ink, CGFloat box_w, CGFloat box_h) {
    CGPathRef path = CTFontCreatePathForGlyph(font, glyph, NULL);
    if (!path) { glyph_place_center_slant(p, ink, box_w, box_h); return; }
    CGFloat k = (CGFloat)SYNTH_ITALIC_SHEAR;
    CGAffineTransform place = CGAffineTransformMake(p->sx, 0, 0, p->sy, p->tx, p->ty);
    CGAffineTransform slant = CGAffineTransformMake(1, 0, k, 1, -k * box_h * 0.5, 0);
    CGAffineTransform t = CGAffineTransformConcat(place, slant);
    CGPathRef leaned = CGPathCreateCopyByTransformingPath(path, &t);
    CGPathRelease(path);
    if (!leaned) { glyph_place_center_slant(p, ink, box_w, box_h); return; }
    CGRect r = CGPathGetPathBoundingBox(leaned);
    CGPathRelease(leaned);
    if (CGRectIsNull(r) || r.size.width <= 0.0) { glyph_place_center_slant(p, ink, box_w, box_h); return; }
    p->tx += box_w * 0.5 - (r.origin.x + r.size.width * 0.5);
}

/* Apply a placement (and a synthesised slant) to the context's transform.
 * The caller brackets this with CGContextSaveGState / RestoreGState.
 *
 * The pooled context draws text with subpixel positioning off, which moves a
 * glyph's origin down to a whole pixel: right for a glyph drawn at its own
 * size (glyph_place moves those by whole pixels), but a glyph scaled into its
 * box or slanted has a fractional origin, and flooring it pushed the ink up to
 * a pixel left and down — out of the cell on that side. Those are drawn where
 * they were placed. Both flags are set for every glyph: the "allows" half is
 * not graphics state, so a restore would leave it on for the glyphs after. */
static void glyph_place_apply(CGContextRef ctx, GlyphPlace p, bool italic,
                              CGFloat box_w, CGFloat box_h) {
    bool exact = italic || p.sx != 1.0 || p.sy != 1.0;
    CGContextSetAllowsFontSubpixelPositioning(ctx, exact);
    CGContextSetShouldSubpixelPositionFonts(ctx, exact);
    if (italic) {
        CGFloat k = (CGFloat)SYNTH_ITALIC_SHEAR;
        CGContextTranslateCTM(ctx, box_w * 0.5, box_h * 0.5);
        CGContextConcatCTM(ctx, CGAffineTransformMake(1, 0, k, 1, 0, 0));
        CGContextTranslateCTM(ctx, -box_w * 0.5, -box_h * 0.5);
    }
    CGContextTranslateCTM(ctx, p.tx, p.ty);
    CGContextScaleCTM(ctx, p.sx, p.sy);
}

static bool ct_rasterize_glyph_m(u8 *buf, i32 cw, i32 ch, u32 cp, f32 ascent_px,
                                 f32 mul, u32 style) {
    if (!g_ct_font) return false;

    /* A private-use icon is the same drawing in every style. */
    if (glyph_fit_kind(cp) >= GLYPH_FIT_ICON) style = 0;
    u8 synth = 0;
    CTFontRef primary = ct_styled_face_locked(style, &synth);

    /* Find which CT font has this glyph — handle surrogate pairs for cp > 0xFFFF */
    CTFontRef font = primary;
    /* Non-NULL only when CoreText returned a fallback that the (full) cache
     * chain could not adopt: this function owns that reference and must release
     * it on every exit path below, after the last use of `font`. */
    CTFontRef owned_fallback = NULL;
    UniChar uchars[2];
    i32 ulen;
    CGGlyph glyphs[2];
    if (cp > 0xFFFF) {
        /* Supplementary plane: encode as UTF-16 surrogate pair */
        uchars[0] = (UniChar)(((cp - 0x10000) >> 10) + 0xD800);
        uchars[1] = (UniChar)(((cp - 0x10000) & 0x3FF) + 0xDC00);
        ulen = 2;
    } else {
        uchars[0] = (UniChar)cp;
        ulen = 1;
    }
    CGGlyph glyph;
    /* A range the codepoint map names is drawn from that face first. */
    CTFontRef mapped = ct_cpmap_font(cp);
    if (mapped && CTFontGetGlyphsForCharacters(mapped, uchars, glyphs, ulen)) {
        font = mapped;
        synth = (u8)(((style & FONT_STYLE_BOLD) ? STYLED_SYNTH_BOLD : 0) |
                     ((style & FONT_STYLE_ITALIC) ? STYLED_SYNTH_ITALIC : 0));
    } else if (!CTFontGetGlyphsForCharacters(font, uchars, glyphs, ulen)) {
        /* The regular face may have what a styled sibling lacks. */
        if (font != g_ct_font && CTFontGetGlyphsForCharacters(g_ct_font, uchars, glyphs, ulen)) {
            font = g_ct_font;
            synth = (u8)(((style & FONT_STYLE_BOLD) ? STYLED_SYNTH_BOLD : 0) |
                         ((style & FONT_STYLE_ITALIC) ? STYLED_SYNTH_ITALIC : 0));
        }
    }
    if (font == primary && !CTFontGetGlyphsForCharacters(font, uchars, glyphs, ulen)) {
        /* A fallback has no styles of its own, so a bold or italic glyph
         * drawn from one is synthesised. */
        synth = (u8)(((style & FONT_STYLE_BOLD) ? STYLED_SYNTH_BOLD : 0) |
                     ((style & FONT_STYLE_ITALIC) ? STYLED_SYNTH_ITALIC : 0));
        font = ct_chain_font_for(cp, uchars, ulen, glyphs, &owned_fallback);
        if (!font) return false;
    }
    glyph = glyphs[0];
    if (font == g_ct_font || font == primary)
        glyph = ct_feature_glyph(font, uchars, ulen, glyph);

    /* Reuse pooled alpha-only bitmap context (OVERSAMPLE× the target size).
     * CTM is scaled by OVERSAMPLE× so glyph coords below are in TARGET cell
     * space. In an AlphaOnly context the fill colour's RGB is ignored;
     * only its alpha is written, so "white at alpha=1" produces full
     * geometric coverage where the glyph paints. */
    CGContextRef ctx = get_glyph_context(cw, ch);
    if (!ctx) {
        if (owned_fallback) CFRelease(owned_fallback);
        return false;
    }

    CGContextClearRect(ctx, CGRectMake(0, 0, cw, ch));
    CGContextSetGrayFillColor(ctx, glyph_fill_grey(), 1.0);

    /* Synthetic emboldening: positive font_weight adds a same-colour stroke
     * around the filled glyph (effectively dilating the outline), and so does
     * a bold glyph drawn from a face that has no bold. Stroke width is in
     * oversampled context units, scaled by FONT_OVERSAMPLE so the user-facing
     * weight value reads as "extra target pixels". */
    f32 stroke = g_font_weight * FONT_OVERSAMPLE;
    if (synth & STYLED_SYNTH_BOLD)
        stroke += synth_bold_width((f32)CTFontGetSize(g_ct_font) * mul) * FONT_OVERSAMPLE;
    bool stroke_apart = glyph_stroke_drawn_apart(stroke);
    if (stroke > 0.01f) {
        CGContextSetGrayStrokeColor(ctx, glyph_fill_grey(), 1.0);
        CGContextSetLineWidth(ctx, (CGFloat)stroke);
        CGContextSetLineJoin(ctx, kCGLineJoinRound);
        CGContextSetTextDrawingMode(ctx, stroke_apart ? kCGTextFill : kCGTextFillStroke);
    } else {
        CGContextSetTextDrawingMode(ctx, kCGTextFill);
    }

    /* A double-width glyph comes from a fallback face (no monospace terminal
     * font ships CJK), and that face is loaded at the FACE size of the primary
     * font, not at the size that fills two terminal columns. An ideograph then
     * inks only part of its box, so a run of them reads as 漢 字 rather than 漢字
     * — the hole is inside the glyph, not between the cells. Scale the face up
     * until the advance fills the box, bounded by the cell height so the glyph
     * cannot grow past its own line. */
    /* Compare against the cell AT THIS RASTER SCALE. A sized glyph (large UI
     * text) also has a cw far above one terminal cell, and scaling it here
     * would double-apply the size it already asked for. */
    bool two_cell_box = (f32)cw > g_cell_width * mul * 1.5f;
    u8 kind = glyph_fit_kind(cp);
    if (two_cell_box && kind < GLYPH_FIT_ICON) {
        CGSize adv0;
        CGRect bb0;
        CTFontGetAdvancesForGlyphs(font, kCTFontOrientationHorizontal, &glyph, &adv0, 1);
        CTFontGetBoundingRectsForGlyphs(font, kCTFontOrientationHorizontal, &glyph, &bb0, 1);
        if (adv0.width > 0.5) {
            f32 fit_w = (f32)cw * 0.96f / ((f32)adv0.width * mul);
            f32 fit_h = (bb0.size.height > 1.0)
                      ? ((f32)ch * 0.90f / ((f32)bb0.size.height * mul)) : 99.0f;
            f32 fit = fit_w < fit_h ? fit_w : fit_h;
            if (fit > 1.8f) fit = 1.8f;     /* never more than a face-size jump */
            if (fit > 1.02f) mul *= fit;    /* ascent_px stays put: the baseline
                                             * must not move relative to Latin */
        }
    }

    /* Draw the face at the target size rather than drawing it small and
     * letting the sampler stretch it — that stretch is exactly what made large
     * headings look soft next to crisp body text. */
    font = ct_scaled_font(font, mul);

    CGSize gadv;
    CGRect ink;
    CTFontGetAdvancesForGlyphs(font, kCTFontOrientationHorizontal, &glyph, &gadv, 1);
    CTFontGetBoundingRectsForGlyphs(font, kCTFontOrientationHorizontal, &glyph, &ink, 1);
    CGFloat pen = font_glyph_pen_x_mb((f32)gadv.width, mul, two_cell_box ? (f32)cw : 0.0f);
    CGFloat base = (CGFloat)ch - ascent_px;
    /* The middle of the capitals, where an icon is centred. */
    CGFloat icon_mid = base + (CGFloat)(g_cap_px * mul) * 0.5;
    CGFloat icon_h = (CGFloat)((two_cell_box ? g_icon2_h_px : g_icon_h_px) * mul);
    if (icon_h <= 0.0) icon_h = (CGFloat)ch;
    /* An icon that took the blank after it keeps a third of that cell empty
     * on its right: the blank is also what parts the icon from the text or
     * the icon that follows, which a wide icon fitted to both cells touches. */
    CGFloat icon_w = (CGFloat)cw;
    if (two_cell_box)
        icon_w -= MAX(1.0, round((CGFloat)(g_cell_width * mul) / 3.0));
    GlyphPlace place = glyph_place(kind, ink, pen, base, (CGFloat)cw, (CGFloat)ch,
                                   icon_w, icon_h, icon_mid, (CGFloat)stroke / FONT_OVERSAMPLE);
    if (synth & STYLED_SYNTH_ITALIC)
        glyph_place_center_slant_path(&place, font, glyph, ink, (CGFloat)cw, (CGFloat)ch);
    CGContextSaveGState(ctx);
    glyph_place_apply(ctx, place, (synth & STYLED_SYNTH_ITALIC) != 0, (CGFloat)cw, (CGFloat)ch);
    CTFontDrawGlyphs(font, &glyph, &(CGPoint){0, 0}, 1, ctx);
    if (stroke_apart) {
        CGContextSetTextDrawingMode(ctx, kCGTextStroke);
        CTFontDrawGlyphs(font, &glyph, &(CGPoint){0, 0}, 1, ctx);
    }
    CGContextRestoreGState(ctx);

    downsample_glyph_buf(buf, cw, ch);
    if (owned_fallback) {
        /* Last use of `font` is above. ct_scaled_font may have keyed its
         * one-slot cache on this pointer (the scaled copy holds its own
         * reference and stays valid), so drop the key before freeing. */
        ct_font_cache_forget(owned_fallback);
        CFRelease(owned_fallback);
    }
    return true;
}

/* Rasterize a color emoji using Core Text — RGBA output.
 *
 * Shaped as a CTLine rather than a single CGGlyph lookup, because a cell can
 * hold several codepoints that only mean one picture once the font's `morx`
 * table has run: without shaping, U+1F468 U+200D U+1F469 draws two people and
 * a flag draws two letters in boxes. CTLine also gives free font fallback for
 * emoji Apple Color Emoji itself does not cover. */
static bool ct_rasterize_color_text(u8 *rgba_buf, i32 cw, i32 ch,
                                    const u8 *text, u32 len, f32 ascent_px) {
    (void)ascent_px;
    CFStringRef str = len ? CFStringCreateWithBytes(kCFAllocatorDefault, text, (CFIndex)len,
                                                   kCFStringEncodingUTF8, false) : NULL;
    if (!str) return false;

    CGFloat size = (CGFloat)(ch * 0.85f);
    CTFontRef emoji_font = CTFontCreateWithName(CFSTR("Apple Color Emoji"), size, NULL);
    if (!emoji_font) { CFRelease(str); return false; }

    CFStringRef keys[]   = { kCTFontAttributeName };
    CFTypeRef   values[] = { emoji_font };
    CFDictionaryRef attrs = CFDictionaryCreate(kCFAllocatorDefault,
                                               (const void **)keys, values, 1,
                                               &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    if (!attrs) { CFRelease(emoji_font); CFRelease(str); return false; }
    CFAttributedStringRef astr = CFAttributedStringCreate(kCFAllocatorDefault, str, attrs);
    CFRelease(attrs);
    if (!astr) { CFRelease(emoji_font); CFRelease(str); return false; }

    CTLineRef line = CTLineCreateWithAttributedString(astr);
    CFRelease(astr);
    if (!line) { CFRelease(emoji_font); CFRelease(str); return false; }

    CGFloat asc = 0, desc = 0, lead = 0;
    double adv = CTLineGetTypographicBounds(line, &asc, &desc, &lead);

    /* An unshaped sequence (an emoji the installed fonts cannot join) comes
     * back far wider than one glyph. Scale it down to fit the two cells rather
     * than letting it run into the neighbouring column. */
    if (adv > (double)cw && adv > 0.0) {
        CGFloat shrunk = size * (CGFloat)((double)cw / adv);
        CTFontRef small = CTFontCreateWithName(CFSTR("Apple Color Emoji"), shrunk, NULL);
        if (small) {
            CFStringRef k2[]   = { kCTFontAttributeName };
            CFTypeRef   v2[]   = { small };
            CFDictionaryRef a2 = CFDictionaryCreate(kCFAllocatorDefault,
                                                    (const void **)k2, v2, 1,
                                                    &kCFTypeDictionaryKeyCallBacks,
                                                    &kCFTypeDictionaryValueCallBacks);
            if (a2) {
                CFAttributedStringRef as2 = CFAttributedStringCreate(kCFAllocatorDefault, str, a2);
                CFRelease(a2);
                if (as2) {
                    CTLineRef l2 = CTLineCreateWithAttributedString(as2);
                    CFRelease(as2);
                    if (l2) {
                        CFRelease(line);
                        line = l2;
                        adv = CTLineGetTypographicBounds(line, &asc, &desc, &lead);
                    }
                }
            }
            CFRelease(small);
        }
    }

    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(rgba_buf, (size_t)cw, (size_t)ch,
                                             8, (size_t)(cw * 4), cs,
                                             (CGBitmapInfo)kCGImageAlphaPremultipliedLast);
    CGColorSpaceRelease(cs);
    if (!ctx) { CFRelease(line); CFRelease(emoji_font); CFRelease(str); return false; }

    /* Clear to transparent */
    CGContextClearRect(ctx, CGRectMake(0, 0, cw, ch));
    CGContextSetAllowsAntialiasing(ctx, true);
    CGContextSetShouldAntialias(ctx, true);

    /* Centre the shaped run in the two-cell box, horizontally on its advance
     * and vertically on its own ascent/descent. */
    CGFloat x_off = ((CGFloat)cw - (CGFloat)adv) / 2.0f;
    if (x_off < 0) x_off = 0;
    CGFloat baseline = ((CGFloat)ch - (asc + desc)) / 2.0f + desc;
    if (baseline < 0) baseline = 0;

    CGContextSetTextPosition(ctx, x_off, baseline);
    CTLineDraw(line, ctx);

    CGContextRelease(ctx);
    CFRelease(line);
    CFRelease(emoji_font);
    CFRelease(str);
    return true;
}

/* Alpha-only sibling for clusters that are NOT emoji — a letter carrying
 * combining marks, an Indic conjunct. Same shaping reason, single cell.
 *
 * The run is set in the face that draws the cluster's BASE on its own — the
 * primary (or its styled face), or the chain's choice for it — so a base with a
 * mark is the same size and design as the base alone; Core Text's cascade
 * still supplies a mark that face lacks. */
static bool ct_rasterize_cluster_text(u8 *buf, i32 cw, i32 ch,
                                      const u8 *text, u32 len, f32 ascent_px,
                                      u32 style) {
    if (!g_ct_font) return false;
    CFStringRef str = len ? CFStringCreateWithBytes(kCFAllocatorDefault, text, (CFIndex)len,
                                                   kCFStringEncodingUTF8, false) : NULL;
    if (!str) return false;

    u8 synth = 0;
    CTFontRef face = ct_styled_face_locked(style, &synth);
    CTFontRef owned = NULL;
    u32 base_cp = 0;
    (void)utf8_decode(text, len, &base_cp);
    CTFontRef mapped = base_cp ? ct_cpmap_font(base_cp) : NULL;
    if (mapped && ct_font_has_cp(mapped, base_cp)) {
        face = mapped;
        synth = (u8)(((style & FONT_STYLE_BOLD) ? STYLED_SYNTH_BOLD : 0) |
                     ((style & FONT_STYLE_ITALIC) ? STYLED_SYNTH_ITALIC : 0));
    } else if (base_cp && !ct_font_has_cp(face, base_cp) && !ct_font_has_cp(g_ct_font, base_cp)) {
        UniChar u[2];
        i32 n = 1;
        if (base_cp > 0xFFFF) {
            u[0] = (UniChar)(((base_cp - 0x10000) >> 10) + 0xD800);
            u[1] = (UniChar)(((base_cp - 0x10000) & 0x3FF) + 0xDC00);
            n = 2;
        } else {
            u[0] = (UniChar)base_cp;
        }
        CGGlyph g[2];
        CTFontRef fb = ct_chain_font_for(base_cp, u, n, g, &owned);
        if (fb) {
            face = fb;
            synth = (u8)(((style & FONT_STYLE_BOLD) ? STYLED_SYNTH_BOLD : 0) |
                         ((style & FONT_STYLE_ITALIC) ? STYLED_SYNTH_ITALIC : 0));
        }
    } else if (base_cp && !ct_font_has_cp(face, base_cp)) {
        face = g_ct_font;
        synth = (u8)(((style & FONT_STYLE_BOLD) ? STYLED_SYNTH_BOLD : 0) |
                     ((style & FONT_STYLE_ITALIC) ? STYLED_SYNTH_ITALIC : 0));
    }

    /* The context's fill, not Core Text's default black: smoothing dilates
     * by the fill's lightness, and the single-glyph path's grey sets it. */
    CFStringRef keys[]   = { kCTFontAttributeName, kCTForegroundColorFromContextAttributeName };
    CFTypeRef   values[] = { face, kCFBooleanTrue };
    CFDictionaryRef attrs = CFDictionaryCreate(kCFAllocatorDefault,
                                               (const void **)keys, values, 2,
                                               &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    if (!attrs) { CFRelease(str); if (owned) CFRelease(owned); return false; }
    CFAttributedStringRef astr = CFAttributedStringCreate(kCFAllocatorDefault, str, attrs);
    CFRelease(attrs);
    CFRelease(str);
    if (!astr) { if (owned) CFRelease(owned); return false; }

    CTLineRef line = CTLineCreateWithAttributedString(astr);
    CFRelease(astr);
    if (!line) { if (owned) CFRelease(owned); return false; }

    /* Same oversampled alpha context the single-glyph path uses, so the cluster
     * lands on the same pixel grid as its neighbours — which means the same
     * lock, too. g_glyph_ctx and g_glyph_buf are ONE shared pair: the async
     * raster worker draws into them under g_ct_mu, and drawing here without it
     * corrupts whatever the worker had in flight. The symptom is glyphs going
     * missing from a line at random (the ones the worker happened to be
     * rasterising while a cluster drew), not a crash. */
    pthread_mutex_lock(&g_ct_mu);
    CGContextRef ctx = get_glyph_context(cw, ch);
    if (!ctx) {
        pthread_mutex_unlock(&g_ct_mu);
        CFRelease(line);
        if (owned) CFRelease(owned);
        return false;
    }
    CGContextClearRect(ctx, CGRectMake(0, 0, cw, ch));
    CGContextSetGrayFillColor(ctx, glyph_fill_grey(), 1.0);
    /* The SAME face-wide adjustments the single-glyph path applies: the
     * synthetic weight (macOS hands back DECOMPOSED filenames, so every
     * accented name is a cluster, as is much of Indic, Thai and Arabic text),
     * a synthesised bold or italic, and the pen that pulls a face whose ink
     * hangs left of its origin back inside the cell. */
    f32 stroke = g_font_weight * FONT_OVERSAMPLE;
    if (synth & STYLED_SYNTH_BOLD)
        stroke += synth_bold_width((f32)CTFontGetSize(g_ct_font)) * FONT_OVERSAMPLE;
    bool stroke_apart = glyph_stroke_drawn_apart(stroke);
    if (stroke > 0.01f) {
        CGContextSetGrayStrokeColor(ctx, glyph_fill_grey(), 1.0);
        CGContextSetLineWidth(ctx, (CGFloat)stroke);
        CGContextSetLineJoin(ctx, kCGLineJoinRound);
        CGContextSetTextDrawingMode(ctx, stroke_apart ? kCGTextFill : kCGTextFillStroke);
    } else {
        CGContextSetTextDrawingMode(ctx, kCGTextFill);
    }
    CGContextSetTextMatrix(ctx, CGAffineTransformIdentity);
    {
        /* Pass the BOX, not zero: a cluster whose base is double-width (NFD
         * Japanese, a CJK ideograph carrying a mark) is rastered into two
         * cells because font_glyph_is_wide answers for clusters, and centring
         * it against one cell would leave it hugging the left edge. A cluster
         * wider than its box is scaled into it rather than cut off. */
        CGFloat adv = (CGFloat)CTLineGetTypographicBounds(line, NULL, NULL, NULL);
        CGRect ink = CTLineGetBoundsWithOptions(line, kCTLineBoundsUseGlyphPathBounds);
        bool two_cell = (f32)cw > g_cell_width * 1.5f;
        CGFloat pen = font_glyph_pen_x_mb((f32)adv, 1.0f, two_cell ? (f32)cw : 0.0f);
        GlyphPlace place = glyph_place(GLYPH_FIT_TEXT, ink, pen, (CGFloat)(ch - ascent_px),
                                       (CGFloat)cw, (CGFloat)ch, (CGFloat)cw, (CGFloat)ch, 0.0,
                                       (CGFloat)stroke / FONT_OVERSAMPLE);
        if (synth & STYLED_SYNTH_ITALIC)
            glyph_place_center_slant(&place, ink, (CGFloat)cw, (CGFloat)ch);
        CGContextSaveGState(ctx);
        glyph_place_apply(ctx, place, (synth & STYLED_SYNTH_ITALIC) != 0, (CGFloat)cw, (CGFloat)ch);
        CGContextSetTextPosition(ctx, 0, 0);
        CTLineDraw(line, ctx);
        if (stroke_apart) {
            /* The first draw left the text matrix where its run ended. */
            CGContextSetTextMatrix(ctx, CGAffineTransformIdentity);
            CGContextSetTextPosition(ctx, 0, 0);
            CGContextSetTextDrawingMode(ctx, kCGTextStroke);
            CTLineDraw(line, ctx);
        }
        CGContextRestoreGState(ctx);
    }
    downsample_glyph_buf(buf, cw, ch);
    /* CTLineDraw leaves the text matrix set to whatever the run needed, and the
     * text matrix is NOT part of the graphics state — CTFontDrawGlyphs on this
     * same pooled context would then place every later glyph through it and
     * draw them off the bitmap. Restoring identity is what keeps the ordinary
     * glyph path working after a cluster passes through. */
    CGContextSetTextMatrix(ctx, CGAffineTransformIdentity);
    pthread_mutex_unlock(&g_ct_mu);
    CFRelease(line);
    if (owned) {
        ct_font_cache_forget(owned);
        CFRelease(owned);
    }
    return true;
}

static bool ct_rasterize_cluster_alpha(u8 *buf, i32 cw, i32 ch, u32 cp, f32 ascent_px,
                                       u32 style) {
    u8 text[UTF8_CLUSTER_MAX * 4];
    u32 len = utf8_cluster_encode(cp, text, sizeof text);
    return ct_rasterize_cluster_text(buf, cw, ch, text, len, ascent_px, style);
}
#endif

/* =========================================================================
 * Fallback font chain — try multiple fonts for missing glyphs
 * ========================================================================= */

/* MAX_FALLBACK_FONTS defined above */

typedef struct {
    stbtt_fontinfo info;
    u8            *data;
    usize          data_size;
    f32            scale;
    bool           loaded;
    char           path[512]; /* font file path for dedup */
#ifdef LIU_HAVE_FREETYPE
    /* FreeType view of the same mapping. NULL when the face would not load or
     * is unscalable, in which case this fallback rasterises through stb. */
    FT_Face        ft;
#endif
} FallbackFont;

static FallbackFont g_fallbacks[MAX_FALLBACK_FONTS];
static i32 g_fallback_count = 0;
static f32 g_pixel_h = 0; /* cached pixel height for dynamic fallback loading */

/* User-configured fallback font paths (set via font_set_user_fallback_config) */
#define MAX_USER_FALLBACK_PATHS 4
static char g_user_fallback_paths[MAX_USER_FALLBACK_PATHS][512];
static i32  g_user_fallback_count = 0;

/* ---- Codepoint-to-fallback-index cache ----
 * Avoids repeated CoreText CTFontCreateForString() calls for the same
 * codepoint. Maps codepoint -> fallback font index (or -1 = primary,
 * -2 = not found). Open-addressing hash table. */

#define CP_FALLBACK_CACHE_SIZE 4096

typedef struct {
    u32 codepoint;  /* 0 = empty slot */
    i16 fb_index;   /* -1 = primary font, -2 = no font found, 0..N = fallback index */
} CpFallbackEntry;

static CpFallbackEntry g_cp_fb_cache[CP_FALLBACK_CACHE_SIZE];

static void cp_fb_cache_clear(void) {
    memset(g_cp_fb_cache, 0, sizeof(g_cp_fb_cache));
}

static i16 cp_fb_cache_lookup(u32 cp) {
    if (cp == 0) return -2;
    u32 h = (cp * 2654435761u) % CP_FALLBACK_CACHE_SIZE;
    for (i32 i = 0; i < 16; i++) {
        u32 slot = (h + (u32)i) % CP_FALLBACK_CACHE_SIZE;
        if (g_cp_fb_cache[slot].codepoint == cp)
            return g_cp_fb_cache[slot].fb_index;
        if (g_cp_fb_cache[slot].codepoint == 0)
            return -2; /* not cached — sentinel for "unknown" */
    }
    return -2;
}

static void cp_fb_cache_insert(u32 cp, i16 fb_index) {
    if (cp == 0) return;
    u32 h = (cp * 2654435761u) % CP_FALLBACK_CACHE_SIZE;
    for (i32 i = 0; i < 16; i++) {
        u32 slot = (h + (u32)i) % CP_FALLBACK_CACHE_SIZE;
        if (g_cp_fb_cache[slot].codepoint == 0 || g_cp_fb_cache[slot].codepoint == cp) {
            g_cp_fb_cache[slot].codepoint = cp;
            g_cp_fb_cache[slot].fb_index = fb_index;
            return;
        }
    }
    /* Table very full — overwrite first probe slot */
    u32 slot = h % CP_FALLBACK_CACHE_SIZE;
    g_cp_fb_cache[slot].codepoint = cp;
    g_cp_fb_cache[slot].fb_index = fb_index;
}

/* Use a distinct sentinel for "looked up but nothing found" vs "not yet looked up" */
#define CP_FB_NOT_CACHED   ((i16)-2)
#define CP_FB_PRIMARY      ((i16)-1)
#define CP_FB_NOT_FOUND    ((i16)-3)

/* Hold an immutable private snapshot: a file-backed MAP_PRIVATE mapping can
 * still reflect later writes/truncation, invalidating the structural gate. */
static u8 *mmap_font_file(const char *path, usize *out_size) {
    if (!path || !*path || !out_size) return NULL;
    *out_size = 0;
#if defined(PLATFORM_MACOS) || defined(PLATFORM_LINUX)
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
        (u64)st.st_size > (u64)MB(256)) { close(fd); return NULL; }
    *out_size = (usize)st.st_size;
    u8 *data = (u8 *)mmap(NULL, *out_size, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (data == MAP_FAILED) { close(fd); return NULL; }
    usize done = 0;
    while (done < *out_size) {
        ssize_t nread = pread(fd, data + done, *out_size - done, (off_t)done);
        if (nread < 0 && errno == EINTR) continue;
        if (nread <= 0) { close(fd); munmap(data, *out_size); return NULL; }
        done += (usize)nread;
    }
    close(fd);
    if (mprotect(data, *out_size, PROT_READ) != 0) {
        munmap(data, *out_size); return NULL;
    }
    return data;
#else
    /* Windows fallback: malloc+read */
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz <= 0 || (u64)sz > (u64)MB(256) || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f); return NULL;
    }
    *out_size = (usize)sz;
    u8 *data = malloc(*out_size);
    if (!data) { fclose(f); return NULL; }
    if (fread(data, 1, *out_size, f) != *out_size) {
        free(data); fclose(f); return NULL;
    }
    fclose(f);
    return data;
#endif
}

static void unmap_font_file(u8 *data, usize size) {
#if defined(PLATFORM_MACOS) || defined(PLATFORM_LINUX)
    if (data) munmap(data, size);
#else
    free(data);
#endif
}

/* ---- Structural gate verdicts --------------------------------------------
 * sfnt_probe() walks every table a glyph can reach, and that is nearly the
 * whole cost of loading a font: reading the file, stbtt_InitFont and the
 * CoreText font are small next to it. Two windows on displays with different
 * backing scales share ONE face, and every glyph first seen in the other
 * window's atlas re-binds it — face_load_locked() runs the gate on the whole
 * chain each time.
 *
 * The gate is a pure function of the bytes, so its verdict is remembered. The
 * key is the CONTENT of the immutable snapshot, never a name or a timestamp:
 * a font file rewritten under the same path (even with its size and mtime put
 * back) is different bytes and is probed again. The hash is keyed with a
 * per-process random value, so no file can be crafted offline to collide with
 * one this process validated. A hit hands back the same verdict and offset the
 * gate produced for those bytes. */
#define FONT_PROBE_MEMO_CAP 16
typedef struct {
    u64   hash;
    usize size;
    u32   offset;
    bool  valid;
    bool  used;
} FontProbeMemo;
static FontProbeMemo   g_probe_memo[FONT_PROBE_MEMO_CAP];
static u32             g_probe_memo_next;
static u64             g_probe_key;
static pthread_mutex_t g_probe_mu = PTHREAD_MUTEX_INITIALIZER;
/* Counts executions of the structural gate. */
static _Atomic u64     g_font_probe_runs;

static u64 probe_rotl(u64 v, int r) { return (v << r) | (v >> (64 - r)); }

/* Four independent lanes reduce dependencies between multiply chains while
 * hashing the input. */
static u64 font_probe_hash(const u8 *p, usize n, u64 key) {
    const u64 K1 = 0x9E3779B97F4A7C15ull, K2 = 0xC2B2AE3D27D4EB4Full,
              K3 = 0x165667B19E3779F9ull;
    u64 a = key, b = key ^ K1, c = key ^ K2, d = ~key;
    usize i = 0;
    for (; i + 32 <= n; i += 32) {
        u64 w0, w1, w2, w3;
        memcpy(&w0, p + i, 8);      memcpy(&w1, p + i + 8, 8);
        memcpy(&w2, p + i + 16, 8); memcpy(&w3, p + i + 24, 8);
        a = probe_rotl((a ^ w0) * K1, 31);
        b = probe_rotl((b ^ w1) * K2, 29);
        c = probe_rotl((c ^ w2) * K3, 27);
        d = probe_rotl((d ^ w3) * K1, 25);
    }
    if (i < n) {
        u8 tail[32] = {0};
        memcpy(tail, p + i, n - i);
        u64 w0, w1, w2, w3;
        memcpy(&w0, tail, 8);      memcpy(&w1, tail + 8, 8);
        memcpy(&w2, tail + 16, 8); memcpy(&w3, tail + 24, 8);
        a = probe_rotl((a ^ w0) * K1, 31);
        b = probe_rotl((b ^ w1) * K2, 29);
        c = probe_rotl((c ^ w2) * K3, 27);
        d = probe_rotl((d ^ w3) * K1, 25);
    }
    u64 h = a ^ probe_rotl(b, 17) ^ probe_rotl(c, 34) ^ probe_rotl(d, 51) ^
            ((u64)n * K2);
    h ^= h >> 30; h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 27; h *= 0x94D049BB133111EBull;
    h ^= h >> 31;
    return h;
}

static u64 font_probe_random_key(void) {
    u64 k = 0;
#if defined(PLATFORM_MACOS) || defined(PLATFORM_LINUX)
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        if (read(fd, &k, sizeof k) != (ssize_t)sizeof k) k = 0;
        close(fd);
    }
#endif
    if (!k) {   /* no entropy device: still unpredictable enough to be per-process */
        k = (u64)(uintptr_t)&k ^ ((u64)getpid() << 32) ^ (u64)clock();
        k *= 0x9E3779B97F4A7C15ull;
    }
    return k | 1u;
}

/* sfnt_probe() with its verdict remembered by content. Same contract: `offset`
 * receives the face's offset when the bytes pass. */
static bool font_probe(const u8 *data, usize size, u32 *offset) {
    if (!data || size == 0) return false;
    pthread_mutex_lock(&g_probe_mu);
    if (!g_probe_key) g_probe_key = font_probe_random_key();
    u64 key = g_probe_key;
    pthread_mutex_unlock(&g_probe_mu);

    u64 h = font_probe_hash(data, size, key);   /* outside the lock */
    pthread_mutex_lock(&g_probe_mu);
    for (u32 i = 0; i < FONT_PROBE_MEMO_CAP; i++) {
        const FontProbeMemo *m = &g_probe_memo[i];
        if (m->used && m->hash == h && m->size == size) {
            bool valid = m->valid;
            if (valid && offset) *offset = m->offset;
            pthread_mutex_unlock(&g_probe_mu);
            return valid;
        }
    }
    pthread_mutex_unlock(&g_probe_mu);

    u32 off = 0;
    bool valid = sfnt_probe(data, size, &off);
    atomic_fetch_add_explicit(&g_font_probe_runs, 1, memory_order_relaxed);

    pthread_mutex_lock(&g_probe_mu);
    FontProbeMemo *slot = &g_probe_memo[g_probe_memo_next++ % FONT_PROBE_MEMO_CAP];
    slot->hash = h; slot->size = size; slot->offset = off;
    slot->valid = valid; slot->used = true;
    pthread_mutex_unlock(&g_probe_mu);
    if (valid && offset) *offset = off;
    return valid;
}

/* The primary face is part of the chain too. */
static bool fallback_path_loaded(const char *path) {
    if (!path || !*path) return false;
    if (g_font_data && strcmp(g_face.path, path) == 0) return true;
    for (i32 i = 0; i < g_fallback_count; i++) {
        if (g_fallbacks[i].loaded && strcmp(g_fallbacks[i].path, path) == 0)
            return true;
    }
    return false;
}

#ifdef PLATFORM_MACOS
/* Create a CTFontRef from a TTF/OTF file. Needed so ct_rasterize_glyph's
 * manual fallback scan finds Liu's bundled fonts (e.g. Symbols Nerd Font
 * Mono) before the generic CTFontCreateForString() path returns an unrelated
 * system font for Private-Use-Area codepoints. */
static CTFontRef ct_font_from_file(const char *path, f32 pixel_h) {
    CFStringRef path_str = CFStringCreateWithCString(NULL, path, kCFStringEncodingUTF8);
    if (!path_str) return NULL;
    CFURLRef url = CFURLCreateWithFileSystemPath(NULL, path_str, kCFURLPOSIXPathStyle, false);
    CFRelease(path_str);
    if (!url) return NULL;
    CGDataProviderRef provider = CGDataProviderCreateWithURL(url);
    CFRelease(url);
    if (!provider) return NULL;
    CGFontRef cg_font = CGFontCreateWithDataProvider(provider);
    CGDataProviderRelease(provider);
    if (!cg_font) return NULL;
    CTFontRef ct = CTFontCreateWithGraphicsFont(cg_font, (CGFloat)pixel_h, NULL, NULL);
    CGFontRelease(cg_font);
    /* A variable face is drawn at its default instance, and some families ship
     * one file whose default is not Regular (a Light master). Choosing the
     * family means its Regular, so the weight axis is set to 400 when it can
     * be. */
    f64 def = 0, mn = 0, mx = 0;
    if (ct && ct_font_wght_axis(ct, &def, &mn, &mx) && fabs(def - 400.0) > 0.5 &&
        mn <= 400.0 && mx >= 400.0) {
        CTFontRef regular = ct_font_at_weight(ct, 400.0);
        if (regular) {
            CFRelease(ct);
            ct = regular;
        }
    }
    return ct;
}

/* Measure a font's advance for a reference glyph (prefers 'M', falls back to
 * a Nerd Font folder icon if the font is symbols-only). Returns 0 on failure. */
static CGFloat ct_font_probe_advance(CTFontRef f) {
    if (!f) return 0;
    UniChar refs[] = { 'M', 0xF07B };
    for (u32 i = 0; i < sizeof(refs)/sizeof(refs[0]); i++) {
        CGGlyph g = 0;
        if (CTFontGetGlyphsForCharacters(f, &refs[i], &g, 1) && g != 0) {
            CGSize adv;
            CTFontGetAdvancesForGlyphs(f, kCTFontOrientationHorizontal, &g, &adv, 1);
            if (adv.width > 0) return adv.width;
        }
    }
    return 0;
}

#endif

/* =========================================================================
 * Lazy fallback loading
 *
 * Fallback fonts are expensive to load — each one creates a CoreText CTFontRef
 * with character maps, metrics tables, shaping features (300-800 KB apiece).
 * Most shell sessions never exercise them (ASCII + primary font covers ~99%
 * of input). Instead of loading all 9 fallbacks at startup, register their
 * paths here and defer the actual load until the first codepoint miss.
 *
 * The one exception is SymbolsNerdFontMono — loaded eagerly because the
 * filebrowser shows Nerd-Font PUA icons on every first frame. Everything else
 * is only touched when a Unicode, CJK, emoji, or symbol glyph is actually
 * used.
 * ========================================================================= */
typedef struct {
    char path[512];
    f32  pixel_h;
    bool attempted;   /* load_fallback was called for this path */
    u8 *cmap_data;
    usize cmap_size;
    stbtt_fontinfo cmap; /* only data/index_map are used by FindGlyphIndex */
} PendingFallback;
#define MAX_PENDING_FALLBACKS 16
static PendingFallback g_pending_fallbacks[MAX_PENDING_FALLBACKS];
static i32 g_pending_fallback_count = 0;

static void queue_lazy_fallback(const char *path, f32 pixel_h) {
    if (!path || !*path) return;
    if (fallback_path_loaded(path)) return;
    for (i32 i = 0; i < g_pending_fallback_count; i++)
        if (strcmp(g_pending_fallbacks[i].path, path) == 0) return;
    if (g_pending_fallback_count >= MAX_PENDING_FALLBACKS) return;
    PendingFallback *p = &g_pending_fallbacks[g_pending_fallback_count++];
    snprintf(p->path, sizeof p->path, "%s", path);
    p->pixel_h = pixel_h;
    p->attempted = false;
}

static bool load_fallback(const char *path, f32 pixel_h); /* fwd decl */

/* Keep only validated character coverage for an unopened candidate. A CJK
 * miss in a Latin face must not retain its full snapshot and CoreText objects.
 * The immutable-file structural gate still runs before any stb reader. */
static bool pending_fallback_has_cp(PendingFallback *p,u32 cp) {
    if (p->cmap_data) return stbtt_FindGlyphIndex(&p->cmap,(int)cp)!=0;
    usize size=0;u8 *data=mmap_font_file(p->path,&size);
    u32 offset=0;stbtt_fontinfo info;
    if (!data) { p->attempted=true;return false; }
    bool valid=font_probe(data,size,&offset) && stbtt_InitFont(&info,data,(int)offset);
    u32 cmap=0,length=0;
    if (valid) {
        u32 tables=sfnt_be16(data+offset+4);
        for(u32 i=0;i<tables;i++) {
            const u8 *record=data+offset+12+16u*i;
            if(sfnt_be32(record)==0x636d6170u) {
                cmap=sfnt_be32(record+8);length=sfnt_be32(record+12);break;
            }
        }
        valid=length>0 && (usize)cmap<=size &&
            (usize)length<=size-(usize)cmap && info.index_map>=(int)cmap &&
            (usize)info.index_map-(usize)cmap<(usize)length;
    }
    bool contains=valid && stbtt_FindGlyphIndex(&info,(int)cp)!=0;
    if (valid && length<=MB(1)) {
        p->cmap_data=malloc(length);
        if (p->cmap_data) {
            memcpy(p->cmap_data,data+cmap,length);p->cmap_size=length;
            p->cmap=(stbtt_fontinfo){.data=p->cmap_data,.index_map=info.index_map-(int)cmap};
            mem_budget_transient_adjust(MEM_FONT_CPU,(isize)length);
        }
    }
    /* A large valid cmap or a failed small-cache allocation can still supply
     * this glyph. The cache limit must not become a font compatibility limit. */
    if (!valid) p->attempted=true;
    unmap_font_file(data,size);
    return contains;
}

/* Open at most one new face; skip unavailable/duplicate paths. Both cmap and
 * CoreText callers check coverage before requesting another. g_fb_mu held. */
static bool load_next_lazy_fallback(u32 cp) {
    for (i32 i = 0; i < g_pending_fallback_count; i++) {
        PendingFallback *p=&g_pending_fallbacks[i];
        if (p->attempted) continue;
        if (fallback_path_loaded(p->path)) {
            p->attempted=true;continue;
        }
        if (!pending_fallback_has_cp(p,cp)) continue;
        p->attempted=true;
        if (load_fallback(p->path,p->pixel_h)) return true;
    }
    return false;
}

static i32 load_lazy_fallback_for_cp(u32 cp) {
    while (load_next_lazy_fallback(cp)) {
        i32 next=g_fallback_count-1;
        if (stbtt_FindGlyphIndex(&g_fallbacks[next].info, (int)cp) != 0)
            return next;
    }
    return -1;
}

static bool load_fallback(const char *path, f32 pixel_h) {
    if (g_fallback_count >= MAX_FALLBACK_FONTS) return false;
    if (fallback_path_loaded(path)) return false; /* already loaded */

    usize sz = 0;
    u8 *data = mmap_font_file(path, &sz);
    if (!data) return false;

    /* Validate the complete reachable TrueType structure; CFF uses bounded
     * readers in the stb fork. Both paths operate on the immutable snapshot. */
    u32 offset = 0;
    if (!font_probe(data, sz, &offset)) { unmap_font_file(data, sz); return false; }
    FallbackFont *fb = &g_fallbacks[g_fallback_count];
    if (!stbtt_InitFont(&fb->info, data, (int)offset)) { unmap_font_file(data, sz); return false; }
    fb->data = data;
    fb->data_size = sz;
    /* Em-relative, matching the primary face (see font_atlas_create) so a
     * fallback glyph is the same size as the surrounding text. */
    fb->scale = stbtt_ScaleForMappingEmToPixels(&fb->info, pixel_h);
    fb->loaded = true;
    snprintf(fb->path, sizeof(fb->path), "%s", path);
#ifdef LIU_HAVE_FREETYPE
    fb->ft = ft_face_from_memory(data, sz);
#endif
    g_fallback_count++;

#ifdef PLATFORM_MACOS
    /* Mirror into CoreText fallback chain so ct_rasterize_glyph sees bundled
     * fonts (Nerd Font PUA icons, in particular) before falling through to
     * CTFontCreateForString. */
    /* At the primary's em, like the stb face above. A glyph that does not fit
     * its box is fitted when it is drawn (glyph_place), one glyph at a time:
     * shrinking the whole face by its widest advance would draw every other
     * glyph in it smaller than the text around it. */
    if (g_ct_fallback_count < MAX_FALLBACK_FONTS) {
        CTFontRef ct = ct_font_from_file(path, pixel_h);
        if (ct) g_ct_fallbacks[g_ct_fallback_count++] = ct;
    }
#endif
    return true;
}

#ifdef PLATFORM_LINUX
/* -------------------------------------------------------------------------
 * System fallback discovery (Linux)
 *
 * There is no CoreText here to ask "which installed font covers this
 * codepoint", and no two distributions agree on where a font file lives:
 * DejaVuSansMono.ttf is /usr/share/fonts/truetype/dejavu/ on Debian,
 * /usr/share/fonts/TTF/ on Arch, and /usr/share/fonts/dejavu-sans-mono-fonts/
 * on Fedora. A hardcoded path list therefore resolves on exactly one distro
 * family and silently leaves everyone else with no CJK, Arabic or emoji
 * coverage at all.
 *
 * So: walk the standard font roots ONCE, matching on basename. One pass over
 * a few hundred directory entries costs well under a millisecond and is
 * distribution-agnostic — no libfontconfig dependency, no subprocess.
 * ------------------------------------------------------------------------- */

/* Ordered by priority: the first entry that exists wins its script's slot,
 * and load_fallback() keeps the chain in insertion order. Latin monospace
 * first (it should win over a CJK face for shared glyphs), then per-script
 * coverage, then emoji last so a text presentation is preferred. */
static const char *const kLinuxFallbackNames[] = {
    /* Latin / general monospace */
    "NotoSansMono-Regular.ttf", "DejaVuSansMono.ttf",
    "LiberationMono-Regular.ttf", "FreeMono.ttf",
    /* CJK — .ttc collections first (they carry all four regional variants) */
    "NotoSansCJK-Regular.ttc", "NotoSansCJKjp-Regular.otf",
    "NotoSansCJKsc-Regular.otf", "NotoSerifCJK-Regular.ttc",
    "SourceHanSansHW-Regular.otf", "wqy-zenhei.ttc",
    /* RTL and Indic */
    "NotoSansArabic-Regular.ttf", "NotoNaskhArabic-Regular.ttf",
    "NotoSansHebrew-Regular.ttf", "NotoSansDevanagari-Regular.ttf",
    "NotoSansThai-Regular.ttf",
    /* Symbols and emoji */
    "NotoSansSymbols2-Regular.ttf", "Symbola.ttf",
    "NotoColorEmoji.ttf", "NotoEmoji-Regular.ttf",
};
#define LINUX_FALLBACK_NAME_COUNT \
    ((i32)(sizeof(kLinuxFallbackNames) / sizeof(kLinuxFallbackNames[0])))

/* Recursive basename scan, depth-limited so a symlink loop or a pathological
 * user font tree can't stall startup. Fills found[i] for any wanted name it
 * has not already resolved (first match wins — roots are searched in
 * user-before-system order, so a user override beats the packaged font). */
static void linux_scan_font_dir(const char *dir, i32 depth,
                                char found[][512]) {
    if (depth > 4) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;   /* skip ., .. and hidden dirs */
        char path[512];
        int n = snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
        if (n <= 0 || (usize)n >= sizeof path) continue;

        /* DT_UNKNOWN happens on filesystems without d_type (some network
         * mounts); fall back to stat rather than skipping the entry. */
        bool is_dir = (de->d_type == DT_DIR);
        bool is_reg = (de->d_type == DT_REG);
        if (de->d_type == DT_UNKNOWN || de->d_type == DT_LNK) {
            struct stat st;
            if (stat(path, &st) != 0) continue;
            is_dir = S_ISDIR(st.st_mode);
            is_reg = S_ISREG(st.st_mode);
        }

        if (is_dir) { linux_scan_font_dir(path, depth + 1, found); continue; }
        if (!is_reg) continue;

        for (i32 i = 0; i < LINUX_FALLBACK_NAME_COUNT; i++) {
            if (found[i][0]) continue;
            if (strcmp(de->d_name, kLinuxFallbackNames[i]) == 0) {
                snprintf(found[i], 512, "%s", path);
                break;
            }
        }
    }
    closedir(d);
}

/* Color emoji source, opened once on first use. Kept out of the stb fallback
 * chain: these fonts have no outlines at all, so stbtt_InitFont rejects them
 * and load_fallback would silently drop them. */
static EmojiFont *g_emoji_font = NULL;
static bool       g_emoji_font_tried = false;
static char       g_emoji_font_path[512];


static EmojiFont *emoji_source(void) {
    if (g_emoji_font_tried) return g_emoji_font;
    g_emoji_font_tried = true;
    if (g_emoji_font_path[0])
        g_emoji_font = emoji_font_open(g_emoji_font_path);
    return g_emoji_font;
}

/* Box-average `src` (sw x sh RGBA) into `dst` (dw x dh RGBA). Emoji bitmaps
 * are authored at ~136x128 and land in a ~22x48 double cell, so this is always
 * a heavy downscale where a box filter is both correct and cheap. Alpha is
 * averaged alongside the colour: the source PNGs are straight (unpremultiplied)
 * RGBA, so edge pixels would otherwise pull colour from fully transparent
 * neighbours and leave a dark halo. */
static void emoji_resample(const u8 *src, i32 sw, i32 sh,
                           u8 *dst, i32 dw, i32 dh) {
    for (i32 y = 0; y < dh; y++) {
        i32 y0 = (i32)((i64)y * sh / dh), y1 = (i32)((i64)(y + 1) * sh / dh);
        if (y1 <= y0) y1 = y0 + 1;
        if (y1 > sh) y1 = sh;
        for (i32 x = 0; x < dw; x++) {
            i32 x0 = (i32)((i64)x * sw / dw), x1 = (i32)((i64)(x + 1) * sw / dw);
            if (x1 <= x0) x1 = x0 + 1;
            if (x1 > sw) x1 = sw;
            u32 a = 0, rr = 0, gg = 0, bb = 0, n = 0;
            for (i32 sy = y0; sy < y1; sy++) {
                const u8 *row = src + ((usize)sy * (usize)sw + (usize)x0) * 4;
                for (i32 sx = x0; sx < x1; sx++, row += 4) {
                    u32 av = row[3];
                    /* Weight colour by coverage so transparent pixels do not
                     * drag the average toward black. */
                    rr += row[0] * av; gg += row[1] * av; bb += row[2] * av;
                    a += av;
                    n++;
                }
            }
            if (!n) n = 1;
            u8 *o = dst + ((usize)y * (usize)dw + (usize)x) * 4;
            if (a == 0) { o[0] = o[1] = o[2] = o[3] = 0; continue; }
            o[0] = (u8)(rr / a);
            o[1] = (u8)(gg / a);
            o[2] = (u8)(bb / a);
            o[3] = (u8)(a / n);
        }
    }
}

static void linux_load_system_fallbacks(f32 em_px) {
    /* The scan result is cached for the process lifetime: font_atlas_create
     * re-runs on every font-size change, DPI change and config reload, and
     * re-walking ~1000 directory entries each time would be pure waste. The
     * cost is that a font installed while Liu is running needs a restart to be
     * picked up — the same deal fontconfig's cache makes. */
    static char found[LINUX_FALLBACK_NAME_COUNT][512];
    static bool scanned = false;
    if (scanned) goto load;
    scanned = true;
    memset(found, 0, sizeof found);

    char user_fonts[512], user_local[512];
    user_fonts[0] = user_local[0] = '\0';
    const char *home = getenv("HOME");
    const char *xdg_data = getenv("XDG_DATA_HOME");
    if (xdg_data && xdg_data[0])
        snprintf(user_fonts, sizeof user_fonts, "%s/fonts", xdg_data);
    else if (home && home[0])
        snprintf(user_fonts, sizeof user_fonts, "%s/.local/share/fonts", home);
    if (home && home[0])
        snprintf(user_local, sizeof user_local, "%s/.fonts", home);

    const char *roots[] = {
        user_fonts,                    /* user-installed wins */
        user_local,                    /* legacy per-user location */
        "/usr/local/share/fonts",
        "/usr/share/fonts",
        "/run/host/fonts",             /* Flatpak: host fonts bind-mounted here */
        "/var/lib/flatpak/exports/share/fonts",
    };
    for (usize i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
        if (roots[i] && roots[i][0]) linux_scan_font_dir(roots[i], 0, found);
    }

load:
    /* The chain is rebuilt on every atlas create (font size / DPI change), so
     * this runs again per rebuild — only the directory scan above is cached. */
    for (i32 i = 0; i < LINUX_FALLBACK_NAME_COUNT; i++) {
        if (!found[i][0]) continue;
        /* Color bitmap fonts go to the CBDT reader, not the outline chain —
         * handing NotoColorEmoji to load_fallback just fails stbtt_InitFont. */
        if (strcmp(kLinuxFallbackNames[i], "NotoColorEmoji.ttf") == 0) {
            snprintf(g_emoji_font_path, sizeof g_emoji_font_path, "%s", found[i]);
            continue;
        }
        load_fallback(found[i], em_px);
    }
}
#ifdef LIU_HAVE_FONTCONFIG
/* -------------------------------------------------------------------------
 * Per-codepoint discovery (Linux, fontconfig)
 *
 * The basename list above names a fixed set of scripts. A character outside
 * them — Armenian, Georgian, Ethiopic, Khmer, Tamil, Cherokee, an obscure
 * symbol block — would render as tofu even when an installed font covers it.
 * This is the same job discover_system_fallback_for_cp() does through
 * CoreText on macOS: ask the system which installed face covers this one
 * codepoint, then load it.
 *
 * FcFontSort rather than FcFontMatch: a match always returns *something*, and
 * that something routinely lacks the character asked about. Sorting gives the
 * candidates in coverage order, so each charset is checked here and the first
 * face that genuinely has the glyph AND that stb_truetype can parse is taken
 * (bitmap-only faces like NotoColorEmoji fail InitFont and must be skipped,
 * not accepted as the answer).
 * ------------------------------------------------------------------------- */
#define FC_DISCOVER_MAX_TRIES 8

static i32 discover_system_fallback_for_cp(u32 cp) {
    if (g_pixel_h <= 0.0f) return -1;

    static bool fc_ready = false;
    static bool fc_ok = false;
    if (!fc_ready) { fc_ready = true; fc_ok = FcInit() == FcTrue; }
    if (!fc_ok) return -1;

    FcCharSet *want = FcCharSetCreate();
    if (!want) return -1;
    if (FcCharSetAddChar(want, (FcChar32)cp) != FcTrue) {
        FcCharSetDestroy(want);
        return -1;
    }

    FcPattern *pat = FcPatternCreate();
    if (!pat) { FcCharSetDestroy(want); return -1; }
    FcPatternAddCharSet(pat, FC_CHARSET, want);
    /* Outlines only: the atlas path rasterises through stb_truetype. */
    FcPatternAddBool(pat, FC_SCALABLE, FcTrue);
    FcConfigSubstitute(NULL, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);

    FcResult res = FcResultNoMatch;
    FcFontSet *set = FcFontSort(NULL, pat, FcTrue /* trim */, NULL, &res);
    FcPatternDestroy(pat);
    FcCharSetDestroy(want);
    if (!set) return -1;

    i32 found = -1;
    i32 tries = 0;
    for (int i = 0; i < set->nfont && tries < FC_DISCOVER_MAX_TRIES; i++) {
        FcCharSet *cover = NULL;
        if (FcPatternGetCharSet(set->fonts[i], FC_CHARSET, 0, &cover) != FcResultMatch ||
            !cover || FcCharSetHasChar(cover, (FcChar32)cp) != FcTrue)
            continue;

        FcChar8 *file = NULL;
        if (FcPatternGetString(set->fonts[i], FC_FILE, 0, &file) != FcResultMatch || !file)
            continue;
        const char *path = (const char *)file;

        /* Already in the chain? Then the linear scan above missed it, which
         * means this face doesn't actually render the codepoint — keep going. */
        for (i32 k = 0; k < g_fallback_count; k++) {
            if (g_fallbacks[k].loaded && strcmp(g_fallbacks[k].path, path) == 0) {
                path = NULL;
                break;
            }
        }
        if (!path) continue;

        tries++;
        i32 new_idx = g_fallback_count;
        if (load_fallback(path, g_pixel_h)) {
            fprintf(stderr, "font: fontconfig fallback '%s' for U+%04X\n", path, cp);
            found = new_idx;
            break;
        }
    }

    FcFontSetDestroy(set);
    return found;
}
#endif /* LIU_HAVE_FONTCONFIG */
#endif /* PLATFORM_LINUX */

#ifndef USE_METAL
/* Lives outside the PLATFORM_LINUX block: the guard that matters is the
 * BACKEND, not the platform. The macOS OpenGL configuration (cmake
 * -DUSE_METAL=OFF) needs this colour texture too, or every emoji would go to
 * the CPU mirror while the GPU sampled an empty texture. Any build without
 * Metal reaches this.
 *
 * Lazily create the RGBA color atlas texture. Deferred to the first emoji so a
 * session that never shows one pays nothing (512x512 RGBA = 1 MB of VRAM).
 * Returns false if the texture could not be created, in which case the caller
 * falls back to the monochrome outline path. */
static bool font_ensure_color_atlas_gl(FontAtlas *atlas) {
    if (atlas->color_texture_gl) return true;
    if (atlas->color_atlas_w <= 0 || atlas->color_atlas_h <= 0) return false;

    GLuint tex = 0;
    glGenTextures(1, &tex);
    if (!tex) return false;
    glBindTexture(GL_TEXTURE_2D, tex);
    /* GL_SRGB8_ALPHA8, not GL_RGBA8.
     *
     * The bytes in this atlas are sRGB-ENCODED (a CoreText / sbix colour glyph
     * is a picture, and every other image texture in the tree declares that:
     * renderer.c's three glTexImage2D calls for images all use GL_SRGB8_ALPHA8,
     * and the Metal side of this very atlas uses RGBA8Unorm_sRGB). GL_RGBA8
     * would hand the shader those bytes as if they were already linear, and
     * with GL_FRAMEBUFFER_SRGB on the driver then encodes them a second time on
     * write: an opaque mid-grey texel of 128 would be written as 188, so OpenGL
     * emoji mid-tones would come out lighter than the same emoji on Metal.
     * Alpha is linear in both formats, so premultiplied coverage is unaffected;
     * the decode applies to RGB only, which is what the shaders multiply. */
    glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8,
                 atlas->color_atlas_w, atlas->color_atlas_h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    /* LINEAR: unlike text coverage, an emoji is a photographic bitmap being
     * scaled down to cell size, and nearest sampling makes it visibly jagged.
     * The Metal backend reaches the same filter through a sampler bound at
     * index 1 (see renderer_flush_glyphs there) rather than through texture
     * state, because Metal has no per-texture filter — the two backends'
     * sampling contract for this atlas is one place, and both say LINEAR. */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    /* The caller rebinds — it uploads into this texture next and restores the
     * alpha atlas afterwards. */

    atlas->color_texture_gl = (u32)tex;
    atlas->color_atlas_x = 0;
    atlas->color_atlas_y = 0;
    atlas->color_row_h = 0;
    font_atlas_reaccount(atlas);
    return true;
}
#endif /* !USE_METAL */

static void free_fallbacks(void) {
    for (i32 i = 0; i < g_fallback_count; i++) {
#ifdef LIU_HAVE_FREETYPE
        /* Before the unmap: the face borrows that buffer. */
        ft_face_destroy(&g_fallbacks[i].ft);
#endif
        unmap_font_file(g_fallbacks[i].data, g_fallbacks[i].data_size);
        g_fallbacks[i].loaded = false;
        g_fallbacks[i].path[0] = '\0';
    }
    g_fallback_count = 0;
    /* Reset the lazy queue so the next atlas-create re-registers with the
     * new pixel_h. Otherwise attempted=true would skip loading them. */
    for (i32 i=0;i<g_pending_fallback_count;i++) {
        mem_budget_transient_adjust(MEM_FONT_CPU,-(isize)g_pending_fallbacks[i].cmap_size);
        free(g_pending_fallbacks[i].cmap_data);
    }
    g_pending_fallback_count = 0;
    memset(g_pending_fallbacks, 0, sizeof(g_pending_fallbacks));
#ifdef PLATFORM_MACOS
    for (i32 i = 0; i < g_ct_fallback_count; i++) {
        if (g_ct_fallbacks[i]) {
            /* A chain font can be the key of either pointer-keyed CT cache
             * (ct_rasterize_glyph_m hands chain fonts to ct_scaled_font). */
            ct_font_cache_forget(g_ct_fallbacks[i]);
            CFRelease(g_ct_fallbacks[i]);
        }
        g_ct_fallbacks[i] = NULL;
    }
    g_ct_fallback_count = 0;
#endif
    cp_fb_cache_clear();
}

#ifdef PLATFORM_MACOS
/* Discover and load a system fallback font for a given codepoint via CoreText.
 * Returns the fallback index in g_fallbacks[], or -1 if not found.
 * This dynamically expands the stb_truetype fallback chain based on what
 * the OS reports as the correct font for a codepoint. */
static i32 discover_system_fallback_for_cp(u32 cp) {
    if (!g_ct_font || g_pixel_h <= 0) return -1;

    /* Encode codepoint as UTF-16 */
    UniChar uchars[2];
    i32 ulen;
    if (cp > 0xFFFF) {
        uchars[0] = (UniChar)(((cp - 0x10000) >> 10) + 0xD800);
        uchars[1] = (UniChar)(((cp - 0x10000) & 0x3FF) + 0xDC00);
        ulen = 2;
    } else {
        uchars[0] = (UniChar)cp;
        ulen = 1;
    }

    CFStringRef str = CFStringCreateWithCharacters(NULL, uchars, ulen);
    if (!str) return -1;

    CTFontRef fallback_ct = CTFontCreateForString(g_ct_font, str, CFRangeMake(0, ulen));
    CFRelease(str);
    if (!fallback_ct) return -1;

    /* Get the file URL of the discovered font */
    CTFontDescriptorRef desc = CTFontCopyFontDescriptor(fallback_ct);
    CFRelease(fallback_ct);
    if (!desc) return -1;

    CFURLRef font_url = CTFontDescriptorCopyAttribute(desc, kCTFontURLAttribute);
    CFRelease(desc);
    if (!font_url) return -1;

    char font_path[512];
    if (!CFURLGetFileSystemRepresentation(font_url, true, (UInt8 *)font_path, sizeof(font_path))) {
        CFRelease(font_url);
        return -1;
    }
    CFRelease(font_url);

    /* Check if this font is already loaded */
    for (i32 i = 0; i < g_fallback_count; i++) {
        if (g_fallbacks[i].loaded && strcmp(g_fallbacks[i].path, font_path) == 0)
            return i;
    }

    /* Try to load as a new fallback */
    i32 new_idx = g_fallback_count;
    if (load_fallback(font_path, g_pixel_h)) {
        fprintf(stderr, "font: auto-discovered fallback '%s' for U+%04X\n", font_path, cp);
        return new_idx;
    }
    return -1;
}
#endif

/* Thread ID for main thread — used to restrict dynamic font discovery
 * to the main thread only (CoreText + load_fallback are not thread-safe). */
static pthread_t g_main_thread;

/* Find which font has the glyph — primary first, then fallbacks.
 * Uses the codepoint-to-fallback cache to skip scanning. On macOS, if
 * no loaded fallback has the glyph, asks CoreText to discover the right
 * system font and dynamically loads it (main thread only).
 *
 * The body runs with g_fb_mu held (see the locking wrapper below) because it
 * reads g_fallbacks / g_cp_fb_cache (shared with the raster worker) and, on the
 * main thread, mutates them via load_lazy_fallback_for_cp / discover. */
static stbtt_fontinfo *find_font_for_glyph_locked(u32 cp, f32 *out_scale) {
    if (!out_scale) return NULL;
    /* g_font_info points INTO the shared mapping — refuse to probe it once the
     * face is gone (failed reload) rather than read unmapped pages. */
    if (!g_font_data) return NULL;
    /* Callers that deliberately fall back to the primary font after a miss
     * still need a defined scale. Negative-cache and discovery failures used
     * to return without initializing this output. */
    *out_scale = g_font_scale;
    /* Check primary font */
    i32 idx = stbtt_FindGlyphIndex(&g_font_info, (int)cp);
    if (idx != 0) {
        *out_scale = g_font_scale;
        return &g_font_info;
    }

    /* Check the codepoint-to-fallback cache */
    i16 cached_fb = cp_fb_cache_lookup(cp);
    if (cached_fb == CP_FB_PRIMARY) {
        *out_scale = g_font_scale;
        return &g_font_info;
    }
    if (cached_fb == CP_FB_NOT_FOUND) {
        return NULL;
    }
    if (cached_fb >= 0 && cached_fb < g_fallback_count && g_fallbacks[cached_fb].loaded) {
        /* Verify the cached result is still valid */
        idx = stbtt_FindGlyphIndex(&g_fallbacks[cached_fb].info, (int)cp);
        if (idx != 0) {
            *out_scale = g_fallbacks[cached_fb].scale;
            return &g_fallbacks[cached_fb].info;
        }
    }

    /* Linear scan of all loaded fallbacks */
    for (i32 i = 0; i < g_fallback_count; i++) {
        if (!g_fallbacks[i].loaded) continue;
        idx = stbtt_FindGlyphIndex(&g_fallbacks[i].info, (int)cp);
        if (idx != 0) {
            *out_scale = g_fallbacks[i].scale;
            cp_fb_cache_insert(cp, (i16)i);
            return &g_fallbacks[i].info;
        }
    }

#ifdef PLATFORM_MACOS
    /* Try unopened candidates in order, stopping at the first matching face. */
    if (g_pending_fallback_count > 0 && pthread_equal(pthread_self(), g_main_thread)) {
        i32 match = load_lazy_fallback_for_cp(cp);
        if (match >= 0) {
            *out_scale = g_fallbacks[match].scale;
            cp_fb_cache_insert(cp, (i16)match);
            return &g_fallbacks[match].info;
        }
    }
#endif

#ifdef PLATFORM_MACOS
    /* System font discovery: ask CoreText to find a font, load it, try again.
     * Main thread only, and NOT because of the shared state — g_fallbacks,
     * g_fallback_count and the codepoint cache are all covered by the g_fb_mu
     * the caller already holds for this whole function. It is CoreText's own
     * discovery (CTFontCreateForString + descriptor copy) that is not safe to
     * run off the main thread. Worker threads reach their glyphs through the
     * CoreText rasterization path (ct_rasterize_glyph), which does its own
     * discovery under g_fb_mu. The fontconfig branch below has no such
     * restriction, which is why it is written separately. */
    if (pthread_equal(pthread_self(), g_main_thread)) {
        i32 discovered = discover_system_fallback_for_cp(cp);
        if (discovered >= 0 && discovered < g_fallback_count) {
            idx = stbtt_FindGlyphIndex(&g_fallbacks[discovered].info, (int)cp);
            if (idx != 0) {
                *out_scale = g_fallbacks[discovered].scale;
                cp_fb_cache_insert(cp, (i16)discovered);
                return &g_fallbacks[discovered].info;
            }
        }
    }
#endif

#ifdef LIU_HAVE_FONTCONFIG
    /* Same idea as the CoreText path above, but WITHOUT its main-thread
     * restriction. That restriction exists because CoreText discovery is not
     * thread-safe; fontconfig's is, and everything this touches — g_fallbacks,
     * g_fallback_count, the codepoint cache — is already serialized by the
     * g_fb_mu the caller holds for this whole function.
     *
     * The restriction must not be copied here: glyphs are rasterized on an
     * async worker, so a codepoint the worker saw FIRST would never get a
     * discovered face and would render as tofu. */
    {
        i32 discovered = discover_system_fallback_for_cp(cp);
        if (discovered >= 0 && discovered < g_fallback_count) {
            idx = stbtt_FindGlyphIndex(&g_fallbacks[discovered].info, (int)cp);
            if (idx != 0) {
                *out_scale = g_fallbacks[discovered].scale;
                cp_fb_cache_insert(cp, (i16)discovered);
                return &g_fallbacks[discovered].info;
            }
        }
    }
#endif

    /* Nothing found — cache the negative result to avoid future lookups */
    cp_fb_cache_insert(cp, CP_FB_NOT_FOUND);
    return NULL;
}

/* Locking wrapper: serializes fallback-chain reads/mutations and the
 * codepoint cache between the render thread and the raster worker. */
static stbtt_fontinfo *find_font_for_glyph(u32 cp, f32 *out_scale) {
    pthread_mutex_lock(&g_fb_mu);
    stbtt_fontinfo *r = find_font_for_glyph_locked(cp, out_scale);
    pthread_mutex_unlock(&g_fb_mu);
    return r;
}

/* Atlas packing state lives in FontAtlasPriv (pack_x / pack_y / pack_row_h) —
 * it addresses one specific texture, so it cannot be process-global. The GL
 * path likewise binds atlas->texture_id directly instead of a cached global
 * name, since texture names are per-context. */

static bool font_ensure_atlas_room(FontAtlas *atlas, i32 w, i32 h);

/* =========================================================================
 * Async glyph rasterization worker
 *
 * A cache miss on a non-trivial glyph would stall the render thread while
 * stb_truetype / CoreText rasterized it. Instead a background pthread drains
 * a request ring, produces an alpha bitmap off-thread, and hands it back to
 * the main thread. The main thread packs it into the atlas + uploads +
 * inserts into the glyph cache at the top of the next frame (GPU APIs are
 * main-thread-only on macOS Metal).
 *
 * Alpha glyphs, color emoji and clusters share this worker. Requests capture
 * cluster bytes and cell geometry on the main thread; completions carry the
 * bitmap kind back to the appropriate atlas packer. Procedurally drawn ranges
 * remain synchronous because their geometry is cheap and a placeholder frame
 * would flash a space where a separator belongs.
 *
 * One worker PER ATLAS (state lives in FontAtlasPriv::raster). With a single
 * process-wide worker and completion ring, every window would drain whatever
 * bitmaps happened to be queued and pack them into its own texture — a glyph
 * requested by window A could land in B's atlas and nowhere else, and
 * stopping the worker on any window close would silence rasterisation for
 * all of them.
 * ========================================================================= */

/* Main-thread-only helpers for the pending set */
static bool pending_contains(RasterState *rs, u32 cp) {
    u32 h = (cp * 2654435761u) % PENDING_SET_CAP;
    for (i32 i = 0; i < 32; i++) {
        u32 slot = (h + i) % PENDING_SET_CAP;
        if (rs->pending_set[slot] == cp) return true;
        /* Deletions leave holes in this tiny open-addressed set. Keep probing
         * the full window or a colliding in-flight codepoint beyond a removed
         * slot becomes invisible and is enqueued twice. */
    }
    return false;
}
static void pending_add(RasterState *rs, u32 cp) {
    if (rs->pending_count >= PENDING_SET_CAP - 32) return;
    u32 h = (cp * 2654435761u) % PENDING_SET_CAP;
    for (i32 i = 0; i < 32; i++) {
        u32 slot = (h + i) % PENDING_SET_CAP;
        if (rs->pending_set[slot] == 0) {
            rs->pending_set[slot] = cp;
            rs->pending_count++;
            return;
        }
        if (rs->pending_set[slot] == cp) return;
    }
}
static void pending_remove(RasterState *rs, u32 cp) {
    u32 h = (cp * 2654435761u) % PENDING_SET_CAP;
    for (i32 i = 0; i < 32; i++) {
        u32 slot = (h + i) % PENDING_SET_CAP;
        if (rs->pending_set[slot] == cp) {
            rs->pending_set[slot] = 0;
            rs->pending_count--;
            return;
        }
    }
}

/* Forward declarations for functions defined later in the file */
static bool sprite_enabled(void);

/* The ONE list of codepoints drawn from the cell geometry (sprite_draw.h):
 * box drawing, block elements, braille, the prompt separators U+E0B0-E0BF,
 * the Symbols for Legacy Computing and the octants. Both raster dispatch sites
 * and the sync/async decision ask it. */
static inline bool sprite_is_procedural(u32 cp) {
    if (!sprite_enabled()) return false;
    return sprite_draw_covers(cp);
}

/* The stroke a procedurally drawn line uses in this atlas: the face's
 * underline thickness, at least a pixel. */
static inline i32 sprite_line(const FontAtlas *atlas) {
    return atlas && atlas->line_px > 0 ? atlas->line_px : 1;
}
#ifdef PLATFORM_MACOS
static bool ct_rasterize_glyph_m(u8 *buf, i32 cw, i32 ch, u32 cp,
                                 f32 ascent_px, f32 mul, u32 style);
#endif

/* Produce an alpha bitmap for a single codepoint. Caller provides a
 * zeroed buffer of size cw*ch. Returns true if something was drawn.
 *
 * IMPORTANT: this function is worker-thread-safe for stb_truetype and box
 * drawing paths. The CoreText path uses g_ct_mu (declared next to the pooled
 * context it guards) around the shared glyph context pool. */

/* Icon fonts can draw ink beyond a glyph's advance. Shrink only glyphs whose
 * overflow would damage their shape; keep cell-filling block and Powerline
 * glyphs unscaled so adjacent cells still tile. Ordinary text is sized to the
 * cell and does not need fitting. */
#define GLYPH_FIT_MAX_SHRINK 0.85f   /* below this, fit; above it, clip */
static bool glyph_fit_to_box(stbtt_fontinfo *font, u32 cp, i32 box_w, i32 box_h,
                             f32 *scale, i32 *x0, i32 *y0, i32 *x1, i32 *y1,
                             i32 *gw, i32 *gh) {
    if (!font || box_w <= 0 || box_h <= 0 || *gw <= 0 || *gh <= 0) return false;
    if (*gw <= box_w && *gh <= box_h) return false;

    f32 fit_w = (f32)box_w / (f32)*gw;
    f32 fit_h = (f32)box_h / (f32)*gh;
    f32 fit = fit_w < fit_h ? fit_w : fit_h;
    if (!(fit > 0.0f) || fit > GLYPH_FIT_MAX_SHRINK) return false;

    *scale *= fit;
    stbtt_GetCodepointBitmapBox(font, (int)cp, *scale, *scale, x0, y0, x1, y1);
    *gw = *x1 - *x0;
    *gh = *y1 - *y0;
    /* Rounding can still leave a pixel over the edge; the caller's clamp
     * handles that, and one pixel is not what this is guarding against. */
    return true;
}

#ifdef LIU_HAVE_FREETYPE
/* Map a face chosen by find_font_for_glyph() back to its FreeType handle.
 * Pointer identity against the array slots, which is safe without the fallback
 * lock: slots are only ever appended, and the one passed in is already loaded,
 * so its `ft` field does not change underneath this. */
static FT_Face ft_face_for_info(const stbtt_fontinfo *info) {
    if (!info) return NULL;
    if (info == &g_font_info) return g_ft_face;
    for (i32 i = 0; i < MAX_FALLBACK_FONTS; i++)
        if (info == &g_fallbacks[i].info) return g_fallbacks[i].ft;
    return NULL;
}

/* Copy a rendered FT bitmap into the R8 cell buffer at (bx, by), clipped to
 * the cell. Handles the two pixel modes the load flags above can produce. */
static void ft_blit(const FT_Bitmap *bm, u8 *buf, i32 cw, i32 ch, i32 bx, i32 by) {
    for (u32 row = 0; row < bm->rows; row++) {
        i32 dy = by + (i32)row;
        if (dy < 0 || dy >= ch) continue;
        const u8 *src = bm->buffer + (i32)row * bm->pitch;
        u8 *dst = buf + dy * cw;
        for (u32 col = 0; col < bm->width; col++) {
            i32 dx = bx + (i32)col;
            if (dx < 0 || dx >= cw) continue;
            u8 v;
            if (bm->pixel_mode == FT_PIXEL_MODE_MONO)
                v = (src[col >> 3] & (0x80 >> (col & 7))) ? 255 : 0;
            else
                v = src[col];
            /* Max, not assign: a combining mark composited over a base glyph
             * arrives here as a second blit into the same cell. */
            if (v > dst[dx]) dst[dx] = v;
        }
    }
}

/* Rasterise one codepoint through FreeType into a zeroed cw*ch R8 buffer.
 *
 * Placement deliberately reproduces the stb path's arithmetic rather than
 * inventing its own: the pen offset comes from the *unhinted* linear advance,
 * which is the same number stb computes, so the horizontal grid does not shift
 * by a hair when this path is compiled in. Only the pixels change.
 *
 * Returns false for anything it cannot handle — no face, no glyph, an
 * unexpected pixel mode — which drops the caller through to stb_truetype. */
/* A bold or italic glyph from a face that has no such style: the outline
 * thickened by the same stroke and slanted by the same shear the Core Text
 * rasteriser uses. Runs between FT_Load_Glyph and FT_Render_Glyph. */
static void ft_synth_style(FT_GlyphSlot slot, u32 style, f64 ppem) {
    if (slot->format != FT_GLYPH_FORMAT_OUTLINE) return;
    if (style & FONT_STYLE_BOLD)
        FT_Outline_Embolden(&slot->outline,
                            (FT_Pos)lround((f64)synth_bold_width((f32)ppem) * 64.0));
    if (style & FONT_STYLE_ITALIC) {
        FT_Matrix m = { 0x10000, (FT_Fixed)lround(SYNTH_ITALIC_SHEAR * 65536.0), 0, 0x10000 };
        FT_Outline_Transform(&slot->outline, &m);
    }
}

static bool ft_rasterize_glyph_m(u32 cp, u8 *buf, i32 cw, i32 ch, f32 mul, u32 style) {
    if (!g_ft_active) return false;

    f32 scale = g_font_scale;
    stbtt_fontinfo *info = find_font_for_glyph(cp, &scale);
    if (!info) return false;
    FT_Face face = ft_face_for_info(info);
    if (!face || face->units_per_EM == 0) return false;

    FT_UInt gi = FT_Get_Char_Index(face, (FT_ULong)cp);
    if (gi == 0) return false;

    /* stb's scale maps the em onto pixels, which is FreeType's ppem. Going
     * through it keeps both rasterisers at one size even after the per-face
     * normalisation load_fallback() applies. */
    f64 ppem = (f64)scale * (f64)mul * (f64)face->units_per_EM;
    if (!(ppem > 0.5) || ppem > 2048.0) return false;

    if (FT_Set_Char_Size(face, 0, (FT_F26Dot6)lround(ppem * 64.0), 72, 72) != 0)
        return false;
    if (FT_Load_Glyph(face, gi, g_ft_load_flags) != 0) return false;
    ft_synth_style(face->glyph, style, ppem);
    if (FT_Render_Glyph(face->glyph, g_ft_render_mode) != 0) return false;

    FT_GlyphSlot slot = face->glyph;
    if (slot->bitmap.pixel_mode != FT_PIXEL_MODE_GRAY &&
        slot->bitmap.pixel_mode != FT_PIXEL_MODE_MONO)
        return false;

    /* Nothing to draw (space, and anything else with an empty outline) is a
     * success: the cell is meant to stay blank. */
    if (slot->bitmap.width == 0 || slot->bitmap.rows == 0) return true;

    /* Oversized icon glyphs get fitted, on the same terms and at the same
     * threshold as the stb path — see glyph_fit_to_box(). */
    bool shrunk = false;
    if ((i32)slot->bitmap.width > cw || (i32)slot->bitmap.rows > ch) {
        f32 fit_w = (f32)cw / (f32)slot->bitmap.width;
        f32 fit_h = (f32)ch / (f32)slot->bitmap.rows;
        f32 fit = fit_w < fit_h ? fit_w : fit_h;
        if (fit > 0.0f && fit <= GLYPH_FIT_MAX_SHRINK) {
            f64 fitted = ppem * (f64)fit;
            if (fitted > 0.5 &&
                FT_Set_Char_Size(face, 0, (FT_F26Dot6)lround(fitted * 64.0), 72, 72) == 0 &&
                FT_Load_Glyph(face, gi, g_ft_load_flags) == 0 &&
                (ft_synth_style(face->glyph, style, fitted), true) &&
                FT_Render_Glyph(face->glyph, g_ft_render_mode) == 0) {
                slot = face->glyph;
                shrunk = true;
            }
            if (slot->bitmap.width == 0 || slot->bitmap.rows == 0) return true;
        }
    }

    i32 gw = (i32)slot->bitmap.width, gh = (i32)slot->bitmap.rows;
    i32 bx, by;
    if (shrunk || ((style & FONT_STYLE_ITALIC) && gw <= cw)) {
        /* A slanted glyph is centred on its own ink, as on Core Text. */
        bx = (cw - gw) / 2;
        by = shrunk ? (ch - gh) / 2 : (i32)(g_font_ascent * mul) - slot->bitmap_top;
    } else {
        /* linearHoriAdvance is 16.16 pixels of the UNHINTED advance — the same
         * quantity as stb's advance*scale, so the pen lands where the stb path
         * puts it. bitmap_left is the hinted bearing, which is the part
         * hinting is meant to move. */
        f32 adv_px = (f32)slot->linearHoriAdvance / 65536.0f;
        /* Pass the BOX: `cw` is two cells for a CJK ideograph or fullwidth
         * form (font_glyph_is_wide doubled it). Handed a zero,
         * font_glyph_pen_x_mb would centre a double-width glyph against ONE
         * cell, leaving it against the left edge of its two-cell box. */
        bx = (i32)floorf((f32)slot->bitmap_left +
                         font_glyph_pen_x_mb(adv_px, mul,
                                             (f32)cw > g_cell_width * mul * 1.5f
                                                 ? (f32)cw : 0.0f));
        by = (i32)(g_font_ascent * mul) - slot->bitmap_top;
    }

    /* Same rescue as the stb path: pull an in-cell glyph back inside the box
     * before clipping takes most of it. */
    if (bx + gw > cw) bx = cw - gw;
    if (by + gh > ch) by = ch - gh;
    if (bx < 0) bx = 0;
    if (by < 0) by = 0;

    ft_blit(&slot->bitmap, buf, cw, ch, bx, by);
    return true;
}
#endif /* LIU_HAVE_FREETYPE */

/* Face-touching half of the raster below; runs with g_face_mu held. */
static bool rasterize_alpha_bitmap_face_locked(u32 cp, u8 *buf, i32 cw, i32 ch, f32 mul,
                                               u32 style) {
    /* stb_truetype, the last resort below, draws the regular face only. */
    (void)style;
#ifdef PLATFORM_MACOS
    /* Core Text writes R8 alpha coverage directly into buf. Serialize on the
     * shared pooled bitmap context. */
    pthread_mutex_lock(&g_ct_mu);
    bool ok = ct_rasterize_glyph_m(buf, cw, ch, cp, g_font_ascent * mul, mul, style);
    pthread_mutex_unlock(&g_ct_mu);
    if (ok) return true;
#endif

    /* g_font_info points INTO the shared mapping; a failed font reload can
     * leave it dangling while workers for the other atlases are still live,
     * so refuse to touch the face rather than read freed pages. */
    if (!g_font_data || g_font_scale == 0.0f) return false;

#ifdef LIU_HAVE_FREETYPE
    /* Hinted raster, when the library is present and the face took. Anything
     * it declines falls through to stb below. */
    pthread_mutex_lock(&g_ft_mu);
    bool ft_ok = ft_rasterize_glyph_m(cp, buf, cw, ch, mul, style);
    pthread_mutex_unlock(&g_ft_mu);
    if (ft_ok) return true;
#endif

    /* stb_truetype fallback — rasterize straight into buf. */
    f32 scale = g_font_scale;
    stbtt_fontinfo *font = find_font_for_glyph(cp, &scale);
    if (!font) return false;
    scale *= mul;
    f32 ascent = g_font_ascent * mul;

    i32 x0, y0, x1, y1;
    stbtt_GetCodepointBitmapBox(font, (int)cp, scale, scale, &x0, &y0, &x1, &y1);
    i32 gw = x1 - x0, gh = y1 - y0;
    bool shrunk = glyph_fit_to_box(font, cp, cw, ch, &scale, &x0, &y0, &x1, &y1, &gw, &gh);
    if (gw > 0 && gh > 0) {
        i32 bx, by;
        if (shrunk) {
            /* Nothing to align to once the glyph is no longer at the font's
             * own size — centre it in the cell. */
            bx = (cw - gw) / 2;
            by = (ch - gh) / 2;
        } else {
            i32 advance_w, lsb;
            stbtt_GetCodepointHMetrics(font, (int)cp, &advance_w, &lsb);
            bx = (i32)floorf(lsb * scale +
                             font_glyph_pen_x_mb(advance_w * scale, mul,
                                                 (f32)cw > g_cell_width * mul * 1.5f
                                                     ? (f32)cw : 0.0f));
            by = (i32)ascent + y0;
        }
        /* A glyph whose ink FITS can still be pushed off the edge by its
         * own placement: the Powerline rounded separators carry a left
         * side bearing of 7-8px inside a 10px cell, and combining marks
         * sit low enough that the cell bottom cuts them. Clipping there
         * drew 2 of 10 columns for U+E0B7 and 3 of 5 rows for U+06ED.
         * Pull the origin back inside the box first — moving a glyph a
         * couple of pixels is invisible next to losing most of it. */
        if (bx + gw > cw) bx = cw - gw;
        if (by + gh > ch) by = ch - gh;
        if (bx < 0) bx = 0;
        if (by < 0) by = 0;
        i32 rw = gw, rh = gh;
        if (bx + rw > cw) rw = cw - bx;
        if (by + rh > ch) rh = ch - by;
        if (rw > 0 && rh > 0) {
            stbtt_MakeCodepointBitmap(font, buf + by * cw + bx,
                                       rw, rh, cw, scale, scale, (int)cp);
        }
    }
    return true;
}

/* Produce a single-channel R8 alpha bitmap for a codepoint. Caller provides a
 * zeroed buffer of size cw*ch. Returns true if something was drawn. Callable
 * from the raster worker, so the face has to be pinned for the duration —
 * another window's font reload swaps it out from under this. */
static bool rasterize_alpha_bitmap_m(const FontAtlas *atlas, u32 cp, u8 *buf,
                                     i32 cw, i32 ch, f32 mul, u32 style) {
    /* Box drawing / block elements — render directly into the R8 buffer.
     * These are drawn from the cell geometry itself, so they are already
     * resolution-independent: no scaled variant is needed or wanted, and no
     * font is touched, so this stays outside the face lock. */
    /* Braille and the Legacy Computing ranges are deliberately NOT pre-warmed
     * at atlas creation the way box drawing is: hundreds of cells of atlas for
     * ranges most users never print, filled on first sight instead. */
    if (sprite_is_procedural(cp) && sprite_draw(cp, buf, cw, ch, sprite_line(atlas)))
        return true;
    pthread_mutex_lock(&g_face_mu);
    bool ok = face_bind_locked(atlas) &&
              rasterize_alpha_bitmap_face_locked(cp, buf, cw, ch, mul, style);
    pthread_mutex_unlock(&g_face_mu);
    return ok;
}

/* Glyphs the grid gives two columns to and that are NOT emoji: CJK, Hangul,
 * fullwidth forms. Emoji get their two-cell box from the colour atlas path.
 * Callers use this for both the raster size and the quad width — they must
 * agree, or the glyph is stretched into (or rattles around inside) the wrong
 * box.
 *
 * A CLUSTER ANSWERS WITH ITS BASE. utf8_char_width measures a cluster by what
 * it renders to, so the GRID gives a CJK-based cluster two columns (ATTR_WIDE)
 * and ui.c emits a two-cell quad; a one-cell raster would be stretched 2x
 * horizontally inside it (U+6F22 + U+0301, for one). Asking utf8_char_width is
 * also the only way the two answers cannot drift, since ATTR_WIDE is set from
 * that same function. */
/* Runs for every bold or italic cell of a row build, on the main thread and on
 * the row-prepare worker alike, so it reads nothing a worker may not: a
 * cluster handle is never looked up here (the intern table belongs to the main
 * thread) and simply carries the bits; the rasteriser strips them again for a
 * cluster that turns out to be an emoji. */
u32 font_style_key(u32 cp, bool bold, bool italic) {
    if ((!bold && !italic) || cp <= 32) return cp;
    if (!utf8_is_cluster(cp)) {
        if (sprite_is_procedural(cp) || utf8_is_emoji(cp)) return cp;
        if ((cp >= 0xE000 && cp <= 0xF8FF) || (cp >= 0xF0000 && cp <= 0x10FFFD)) return cp;
    }
    return cp | (bold ? FONT_STYLE_BOLD : 0u) | (italic ? FONT_STYLE_ITALIC : 0u);
}

bool font_style_face(FontAtlas *atlas, u32 style, char *name, usize n, bool *synthesised) {
    if (name && n) name[0] = '\0';
    if (synthesised) *synthesised = false;
#ifdef PLATFORM_MACOS
    pthread_mutex_lock(&g_face_mu);
    bool ok = face_bind_locked(atlas) && g_ct_font;
    if (ok) {
        u8 synth = 0;
        CTFontRef f = ct_styled_face_locked(style & FONT_STYLE_MASK, &synth);
        CFStringRef ps = f ? CTFontCopyPostScriptName(f) : NULL;
        if (ps && name && n) CFStringGetCString(ps, name, (CFIndex)n, kCFStringEncodingUTF8);
        if (ps) CFRelease(ps);
        if (synthesised) *synthesised = synth != 0;
        ok = f != NULL;
    }
    pthread_mutex_unlock(&g_face_mu);
    return ok;
#else
    (void)atlas; (void)style;
    return false;
#endif
}

bool font_glyph_is_wide(u32 cp) {
    /* Emoji first: utf8_char_width says 2 for them too, but their two cells
     * come from the colour atlas's own box, not from doubling this one. */
    if (utf8_is_emoji(cp)) return false;
    return utf8_char_width(cp) == 2;
}

/* See renderer.h. Kept beside the width query because callers reach for the
 * two together when deciding which raster path a codepoint takes. */
bool font_color_glyph_available(u32 cp) {
#ifdef PLATFORM_MACOS
    /* CTLine resolves emoji across the whole system fallback chain, so there
     * is no single font whose repertoire could be short. */
    (void)cp;
    return true;
#elif defined(PLATFORM_LINUX)
    pthread_mutex_lock(&g_face_mu);
    EmojiFont *f = emoji_source();
    bool available = f && emoji_font_has_glyph(f, cp);
    pthread_mutex_unlock(&g_face_mu);
    return available;
#else
    (void)cp;
    return false;
#endif
}

static bool rasterize_alpha_bitmap(const FontAtlas *atlas, u32 cp, u8 *buf,
                                   i32 cw, i32 ch, u32 style) {
    return rasterize_alpha_bitmap_m(atlas, cp, buf, cw, ch, 1.0f, style);
}

static bool color_glyph_fill_rgba_payload(u8 *rgba, i32 w, i32 h, u32 cp,
                                          const u8 *text, u32 len);
static CachedGlyph *color_bitmap_install(FontAtlas *atlas, u32 key,
                                          const u8 *rgba, i32 w, i32 h);

/* Bitmap allocation and shaping only. Every lookup that classifies the cell is
 * captured before enqueue, so no worker reads the cluster intern table. */
static u8 *raster_request_bitmap(FontAtlas *atlas, const RasterRequest *req,
                                 i32 *width, u8 *kind, bool *ok) {
    *width = req->w; *kind = FONT_GLYPH_ALPHA; *ok = false;
    usize pixels, bytes;
    if (req->w <= 0 || req->h <= 0) return NULL;
    i32 cc = req->color_cells == 1 ? 1 : 2;
    if (req->color && req->w <= INT_MAX / 2 &&
        checked_mul_usize((usize)req->w * (usize)cc, (usize)req->h, &pixels) &&
        checked_mul_usize(pixels, 4u, &bytes)) {
        u8 *rgba = calloc(1, bytes);
        if (rgba) {
            pthread_mutex_lock(&g_face_mu);
            *ok = face_bind_locked(atlas) &&
                color_glyph_fill_rgba_payload(rgba, req->w * cc, req->h, req->cp,
                                              req->text, req->text_len);
            pthread_mutex_unlock(&g_face_mu);
            if (*ok) { *width = req->w * cc; *kind = FONT_GLYPH_COLOR; return rgba; }
            free(rgba);
        }
    }
    if (!checked_mul_usize((usize)req->w, (usize)req->h, &pixels) ||
        !checked_mul_usize(pixels, ATLAS_BPP, &bytes)) return NULL;
    u8 *alpha = calloc(1, bytes);
    if (!alpha) return NULL;
#ifdef PLATFORM_MACOS
    if (req->cluster) {
        pthread_mutex_lock(&g_face_mu);
        *ok = face_bind_locked(atlas) &&
              ct_rasterize_cluster_text(alpha, req->w, req->h, req->text,
                                        req->text_len, g_font_ascent, req->style);
        pthread_mutex_unlock(&g_face_mu);
        if (*ok) return alpha;
        memset(alpha, 0, bytes);
    }
#endif
    *ok = rasterize_alpha_bitmap(atlas, req->base, alpha, req->w, req->h, req->style);
    return alpha;
}

/* Told, from the worker, that a completion is waiting. Process-wide; see
 * font_set_raster_wake(). */
static _Atomic(void (*)(void)) g_raster_wake;

void font_set_raster_wake(void (*wake)(void)) {
    atomic_store_explicit(&g_raster_wake, wake, memory_order_release);
}

static void *raster_worker_main(void *arg) {
    FontAtlas *atlas = (FontAtlas *)arg;
    RasterState *rs = &fa_priv(atlas)->raster;
    for (;;) {
        pthread_mutex_lock(&rs->req_mu);
        while (rs->req_head == rs->req_tail &&
               !atomic_load_explicit(&rs->stop, memory_order_acquire))
            pthread_cond_wait(&rs->req_cv, &rs->req_mu);
        if (atomic_load_explicit(&rs->stop, memory_order_acquire)) {
            pthread_mutex_unlock(&rs->req_mu); break;
        }
        RasterRequest req = rs->req_ring[rs->req_head];
        rs->req_head = (rs->req_head + 1) % RASTER_REQ_CAP;
        pthread_mutex_unlock(&rs->req_mu);

        /* Every dequeued codepoint MUST come back as a completion, even a
         * failed one: pending_remove() runs in the drain, so a request that
         * returns nothing would be stranded in pending_set for the life of
         * the atlas. The main loop keeps painting while pending_count > 0
         * (see font_raster_work_pending), so one stranded codepoint would
         * pin the loop at the panel rate forever. */
        i32 cw = req.w, ch = req.h;
        bool ok = false;
        u8 kind = FONT_GLYPH_ALPHA;
        u8 *bmp = raster_request_bitmap(atlas, &req, &cw, &kind, &ok);

        pthread_mutex_lock(&rs->done_mu);
        i32 dnext = (rs->done_tail + 1) % RASTER_DONE_CAP;
        /* If the completion ring is full, wait for the main thread to drain it
         * instead of dropping the result. Dropping stranded the codepoint in
         * the main thread's pending_set forever (it's never re-queued), so the
         * glyph would never render. The bitmap is already rasterized; just hold
         * it until there is room. */
        while (dnext == rs->done_head &&
               !atomic_load_explicit(&rs->stop, memory_order_acquire)) {
            pthread_cond_wait(&rs->done_cv, &rs->done_mu);
            dnext = (rs->done_tail + 1) % RASTER_DONE_CAP;
        }
        if (atomic_load_explicit(&rs->stop, memory_order_acquire)) {
            free(bmp);
            pthread_mutex_unlock(&rs->done_mu);
            break;
        }
        RasterCompletion *rc = &rs->done_ring[rs->done_tail];
        rc->cp = req.cp;
        rc->bitmap = bmp;
        rc->w = cw;
        rc->h = ch;
        rc->ok = ok;
        rc->kind = kind;
        rs->done_tail = dnext;
        pthread_mutex_unlock(&rs->done_mu);
        /* AFTER the publish and the unlock: the frame this wakes must find the
         * completion in the ring. The platform coalesces repeated wakes. */
        void (*wake)(void) = atomic_load_explicit(&g_raster_wake, memory_order_acquire);
        if (wake) wake();
    }
    return NULL;
}

void font_start_async_raster(FontAtlas *atlas) {
    FontAtlasPriv *p = fa_priv(atlas);
    if (!p || !p->raster.mu_ready || p->raster.started) return;
    RasterState *rs = &p->raster;
    atomic_store_explicit(&rs->stop, false, memory_order_release);
    rs->req_head = rs->req_tail = 0;
    rs->done_head = rs->done_tail = 0;
    rs->pending_count = 0;
    memset(rs->pending_set, 0, sizeof(rs->pending_set));
    /* Raster worker handles small glyph rasterization requests — doesn't need
     * the default 8 MB pthread stack. Cap at 512 KB to save virtual VM range
     * and keep the stack hot in cache. */
    pthread_attr_t attr;
    pthread_attr_t *pattr = NULL;
    if (pthread_attr_init(&attr) == 0) {
        pthread_attr_setstacksize(&attr, 512 * 1024);
        pattr = &attr;
    }
    if (pthread_create(&rs->thread, pattr, raster_worker_main, atlas) == 0) {
        rs->started = true;
    }
    if (pattr) pthread_attr_destroy(pattr);
}

void font_stop_async_raster(FontAtlas *atlas) {
    FontAtlasPriv *p = fa_priv(atlas);
    if (!p || !p->raster.started) return;
    RasterState *rs = &p->raster;
    pthread_mutex_lock(&rs->req_mu);
    atomic_store_explicit(&rs->stop, true, memory_order_release);
    pthread_cond_broadcast(&rs->req_cv);
    pthread_mutex_unlock(&rs->req_mu);
    /* Also wake a worker blocked waiting for done-ring space so it can observe
     * stop and exit — otherwise pthread_join below would hang. */
    pthread_mutex_lock(&rs->done_mu);
    pthread_cond_broadcast(&rs->done_cv);
    pthread_mutex_unlock(&rs->done_mu);
    pthread_join(rs->thread, NULL);
    /* Drain any leftover completions to avoid bitmap leaks */
    pthread_mutex_lock(&rs->done_mu);
    for (i32 i = rs->done_head; i != rs->done_tail; i = (i + 1) % RASTER_DONE_CAP)
        free(rs->done_ring[i].bitmap);
    rs->done_head = rs->done_tail = 0;
    pthread_mutex_unlock(&rs->done_mu);
    rs->started = false;
}

/* Is there glyph work in flight whose result is not on screen yet?
 *
 * A codepoint seen for the first time is handed to the raster worker and
 * font_get_glyph_rect returns the SPACE glyph's UVs meanwhile — so the frame
 * that discovered it draws a hole, and only a LATER frame (the one whose
 * font_drain_raster_completions() installs it and invalidates the row caches)
 * shows the real character. That frame has to be asked for: with cursor blink
 * off, an idle window produces no frames at all, and a TUI that repaints only
 * when the user types produces one per keystroke, so the hole would stay on
 * screen — text with gaps in it, words that never arrive.
 *
 * The loop asks this through liu_continuous_visual() and keeps painting until
 * the queue is empty. pending_count is main-thread-only state (raster_enqueue
 * adds, the drain removes), so no lock is needed. */
/* Reset before a row, read after it: did anything in between draw as the
 * placeholder because the worker has not finished with it yet? */
void font_placeholder_reset(FontAtlas *atlas) {
    FontAtlasPriv *p = fa_priv(atlas);
    if (p) p->served_placeholder = false;
}

bool font_placeholder_seen(const FontAtlas *atlas) {
    const FontAtlasPriv *p = fa_priv((FontAtlas *)atlas);
    return p && p->served_placeholder;
}

bool font_raster_work_pending(const FontAtlas *atlas) {
    FontAtlasPriv *p = fa_priv(atlas);
    if (!p || !p->raster.started) return false;
    return p->raster.pending_count > 0;
}

bool font_raster_frame_owed(const FontAtlas *atlas) {
    FontAtlasPriv *p = fa_priv(atlas);
    if (!p || !p->raster.started) return false;
    if (!atomic_load_explicit(&g_raster_wake, memory_order_acquire))
        return p->raster.pending_count > 0;   /* no wake will come: keep polling */
    RasterState *rs = &p->raster;
    pthread_mutex_lock(&rs->done_mu);
    bool ready = rs->done_head != rs->done_tail;
    pthread_mutex_unlock(&rs->done_mu);
    return ready;
}

/* Enqueue a codepoint for async rasterization. Main thread only. */
static void raster_enqueue(FontAtlas *atlas, FontAtlasPriv *p, u32 cp) {
    RasterState *rs = &p->raster;
    if (!rs->started) return;
    if (pending_contains(rs, cp)) return;

    /* `cp` is the cache key; the style and icon bits ride beside the codepoint. */
    u32 plain = cp & ~FONT_KEY_FLAGS;
    RasterRequest req = {.cp = cp, .base = utf8_cluster_base(plain),
                         .style = cp & FONT_STYLE_MASK,
                         .w = (i32)atlas->cell_width, .h = (i32)atlas->cell_height,
                         .cluster = utf8_is_cluster(plain),
                         .color = utf8_is_emoji(plain) && atlas->color_atlas_w > 0};
    if (font_glyph_is_wide(plain) || (cp & FONT_ICON_WIDE)) req.w *= 2;
    req.color_cells = utf8_char_width(plain) >= 2 ? 2 : 1;
    req.text_len = utf8_cluster_encode(plain, req.text, sizeof req.text);
    pthread_mutex_lock(&rs->req_mu);
    i32 next = (rs->req_tail + 1) % RASTER_REQ_CAP;
    if (next == rs->req_head) {
        /* Request ring full — drop this request; it'll be retried next frame. */
        pthread_mutex_unlock(&rs->req_mu);
        return;
    }
    rs->req_ring[rs->req_tail] = req;
    rs->req_tail = next;
    pthread_cond_signal(&rs->req_cv);
    pthread_mutex_unlock(&rs->req_mu);

    pending_add(rs, cp);
}

/* Defined below — forward declare for drain. */
static CachedGlyph *cache_insert(FontAtlasPriv *p, u32 cp, f32 x0, f32 y0, f32 x1, f32 y1, u8 kind);
static void cache_insert_exhausted(FontAtlas *atlas, u32 cp);  /* atlas-full sentinel */

/* THE one way texels reach the GPU from this file. Bumps the content version
 * and hands the region to the backend: on Metal the renderer copies it into
 * its staging buffer and blits it at the next frame's preparation, behind
 * every frame already submitted (renderer_atlas_upload); with no renderer
 * attached (headless tests, the seed before the first frame) the write is
 * immediate. On OpenGL the command stream orders glTexSubImage2D itself, so
 * the write is immediate AND counts as ready at once. `tex` is the alpha or
 * colour texture handle; `gl_tex` its GL name; `gl_fmt` GL_RED or GL_RGBA. */
#ifdef USE_METAL
static id<MTLTexture> font_metal_new_atlas_texture(void *device, MTLPixelFormat fmt,
                                                   i32 w, i32 h);
static bool font_metal_ensure_private_alpha(FontAtlas *atlas) {
    if(!atlas->alpha_shared)return true;
    id<MTLTexture> texture=font_metal_new_atlas_texture(atlas->metal_device,MTLPixelFormatR8Unorm,
                                                       atlas->atlas_width,atlas->atlas_height);
    if(!texture)return false;
    FontAtlasPriv *p=fa_priv(atlas);
    if(p && p->gpu_flush)p->gpu_flush(p->gpu_sync_ctx);
    if(atlas->gpu_uploader)
        renderer_atlas_texture_replaced(atlas->gpu_uploader,FONT_GLYPH_ALPHA,NULL,(void *)texture,0,0);
    else renderer_metal_texture_clear_sync((void *)texture);
    id old=(id)atlas->metal_texture;[old release];[old release];
    atlas->metal_texture=(void *)texture;atlas->alpha_shared=false;
    if(p){p->content_version++;if(!atlas->gpu_gated)atlas->gpu_ready_version=p->content_version;}
    font_atlas_reaccount(atlas);return true;
}
#endif

/* False when the renderer REFUSED the write (a staging allocation failed, a
 * region did not fit): nothing reached the GPU and the content version does
 * not move, so no cache entry may be made for the texels — the caller leaves
 * the glyph uncached and the next lookup rasterises it again. */
static bool font_gpu_upload(FontAtlas *atlas, void *tex, u32 gl_tex, u32 gl_fmt,
                            i32 x, i32 y, i32 w, i32 h,
                            const void *data, usize bytes) {
    FontAtlasPriv *p = fa_priv(atlas);
    if (!p) return false;
#ifdef USE_METAL
    if(atlas->alpha_shared && tex==atlas->metal_texture) {
        if(!font_metal_ensure_private_alpha(atlas))return false;
        tex=atlas->metal_texture;
    }
#endif
    p->content_version++;
#ifdef USE_METAL
    (void)gl_tex; (void)gl_fmt;
    if (tex && !renderer_atlas_upload(atlas->gpu_uploader, tex, x, y, w, h, data, bytes,
                                      p->content_version)) {
        p->content_version--;
        p->upload_refused++;
        return false;
    }
    if (!atlas->gpu_gated) atlas->gpu_ready_version = p->content_version;
#else
    (void)tex; (void)bytes;
    if (gl_tex) {
        GLint prev = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
        glBindTexture(GL_TEXTURE_2D, (GLuint)gl_tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, gl_fmt == GL_RGBA ? 4 : 1);
        glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, gl_fmt, GL_UNSIGNED_BYTE, data);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        /* Restore rather than assume the alpha atlas: this runs mid-frame from
         * the synchronous raster path, with whatever the renderer had bound. */
        glBindTexture(GL_TEXTURE_2D, (GLuint)prev);
    }
    atlas->gpu_ready_version = p->content_version;
#endif
    return true;
}
/* Is this glyph's content prepared for the GPU the current frame samples? */
static _Thread_local const FontAtlas *s_batch_atlas;
static inline bool glyph_gpu_ready(const FontAtlas *atlas, u32 content_version) {
    return s_batch_atlas == atlas || !atlas->gpu_gated || content_version <= atlas->gpu_ready_version;
}

/* Defined with the atlas packer below; the drain and the synchronous
 * rasteriser both reach for it when the shelf runs out. */
static bool atlas_reclaim_shelf(FontAtlas *atlas);
/* The reclaim itself, with no GPU gate in front of it. atlas_reclaim_shelf()
 * decides WHETHER it may run; font_atlas_service_reclaim() runs the deferred
 * one once the GPU has been shown to be finished with the cells. */
static bool atlas_reclaim_perform(FontAtlas *atlas);

/* Drain completed rasterizations: pack into atlas, upload, insert into cache.
 * Called from the main thread at frame start. Returns the number of newly
 * installed glyphs — callers use this to invalidate downstream row caches. */
u32 font_drain_raster_completions(FontAtlas *atlas) {
    FontAtlasPriv *p = fa_priv(atlas);
    if (!p) return 0;
    /* Tick the LRU clock once per frame. Called from the main thread at frame
     * start regardless of whether any glyphs are pending. Per atlas: a global
     * clock advances once per WINDOW per frame, so an idle window's entries
     * age N times too fast and get evicted while still on screen. */
    p->glyph_frame++;

    RasterState *rs = &p->raster;
    if (!rs->started) return 0;

    /* 64, not 16. Every drain that installs a glyph invalidates EVERY row
     * cache in every tab (see the caller), so a small batch does not spread
     * the work — it multiplies the full rebuilds, and it stretches the window
     * during which the not-yet-installed glyphs are still drawn as spaces
     * across four times as many frames. */
    RasterCompletion batch[64];
    i32 take = 0;
    pthread_mutex_lock(&rs->done_mu);
    while (take < 64 && rs->done_head != rs->done_tail) {
        batch[take++] = rs->done_ring[rs->done_head];
        rs->done_head = (rs->done_head + 1) % RASTER_DONE_CAP;
    }
    /* Wake the worker if it was blocked waiting for room in the done ring. */
    if (take > 0) pthread_cond_broadcast(&rs->done_cv);
    pthread_mutex_unlock(&rs->done_mu);

    u32 installed = 0;
    for (i32 i = 0; i < take; i++) {
        RasterCompletion *rc = &batch[i];
        pending_remove(rs, rc->cp);
        if (!rc->ok || !rc->bitmap) { free(rc->bitmap); continue; }

        if (rc->kind == FONT_GLYPH_COLOR) {
            if (color_bitmap_install(atlas, rc->cp, rc->bitmap, rc->w, rc->h)) installed++;
            free(rc->bitmap);
            continue;
        }

        /* Atlas packing — main-thread-only state */
        i32 cw = rc->w, ch = rc->h;
        if (!font_ensure_atlas_room(atlas, cw, ch) &&
            !(atlas_reclaim_shelf(atlas) &&
              font_ensure_atlas_room(atlas, cw, ch))) {
            free(rc->bitmap); /* atlas full and reclamation would thrash */
            /* Record an atlas-exhausted sentinel so font_get_glyph_rect stops
             * re-enqueuing this codepoint to the raster worker every frame.
             * Without it, cache_lookup keeps missing, raster_enqueue keeps
             * firing, and the worker keeps rasterising a glyph that can never
             * be packed — sustained background CPU. */
            cache_insert_exhausted(atlas, rc->cp);
            continue;
        }
        i32 ax = p->pack_x + 1, ay = p->pack_y + 1;

        atlas_mirror_r8(atlas, ax, ay, cw, ch, rc->bitmap);
        if (!font_gpu_upload(atlas, atlas->metal_texture, atlas->texture_id, GL_RED,
                             ax, ay, cw, ch, rc->bitmap, (usize)cw * (usize)ch * ATLAS_BPP)) {
            /* Refused: no entry, no advance. The codepoint is off pending_set
             * already, so its next lookup re-queues it and the slot is reused. */
            free(rc->bitmap);
            continue;
        }

        p->pack_x += cw + 2;
        if (ch + 2 > p->pack_row_h) p->pack_row_h = ch + 2;
        p->packed_since_reset++;

        cache_insert(p, rc->cp, (f32)ax, (f32)ay, (f32)(ax + cw), (f32)(ay + ch),
                     FONT_GLYPH_ALPHA);
        free(rc->bitmap);
        installed++;
    }

    if (installed > 0) rs->completed_version++;
    return installed;
}

u32 font_raster_completion_version(const FontAtlas *atlas) {
    const FontAtlasPriv *p = fa_priv(atlas);
    return p ? p->raster.completed_version : 0;
}

bool font_atlas_reclaim_pending(const FontAtlas *atlas) {
    const FontAtlasPriv *p = atlas ? (const FontAtlasPriv *)atlas->priv : NULL;
    return p && p->reclaim_wanted;
}

/* Clear the pending request and its wait state. Called on every path that
 * either performs the reclaim or abandons it, so stale wait state can never
 * gate the next one. */
static void atlas_reclaim_clear_request(FontAtlasPriv *p) {
    p->reclaim_wanted        = false;
    p->reclaim_turns         = 0;
    p->reclaim_quiescing     = false;
    p->reclaim_quiesce_since = 0.0;
}

FontReclaimState font_atlas_service_reclaim(FontAtlas *atlas, f64 now) {
    FontAtlasPriv *p = atlas ? fa_priv(atlas) : NULL;
    if (!p || !p->reclaim_wanted) return FONT_RECLAIM_NONE;

    /* Without a GPU, no device can be reading the atlas cells, so reclamation is
     * safe to perform immediately. */
    if (!p->gpu_idle) {
        atlas_reclaim_clear_request(p);
        return atlas_reclaim_perform(atlas) ? FONT_RECLAIM_DONE : FONT_RECLAIM_NONE;
    }

    /* THE BARRIER, ASKED AT THE INSTANT IT IS USED.
     *
     * Two things have to be true together, and only here are they: the caller
     * promises no encoder is open, and the GPU reports that every frame it was
     * given has finished. Between them there is no reader of the atlas anywhere,
     * so the cells can be handed back and the next frame — created after this
     * returns — measures its UVs against the new layout.
     *
     * A mark taken on an earlier attempt and re-asked later is not enough: its
     * passing means "everything submitted BEFORE it is done" and says nothing
     * about the frames submitted since, which read the same atlas. */
    if (p->gpu_idle(p->gpu_sync_ctx)) {
        atlas_reclaim_clear_request(p);
        return atlas_reclaim_perform(atlas) ? FONT_RECLAIM_DONE : FONT_RECLAIM_NONE;
    }

    /* Phase A: wait. An event-driven terminal stops submitting as soon as the
     * output burst ends, so the queue usually empties on its own within a couple
     * of turns and no frame is dropped at all. Nothing is reclaimed meanwhile, so
     * the frames painted during this phase are free to read the atlas. */
    if (++p->reclaim_turns <= FONT_RECLAIM_IDLE_TURNS) return FONT_RECLAIM_WAIT;

    /* Phase B: make it happen. Something is producing frames continuously, so ask
     * the caller to stop; with nothing new submitted the queue drains within the
     * in-flight depth and phase A's test succeeds on one of the next turns.
     *
     * No blocking drain here: a waitUntilCompleted on the UI thread would stall
     * exactly under the load that makes it expensive. Withholding frames costs
     * the picture a few vblanks and costs the event loop nothing: input and every
     * PTY keep being serviced throughout. */
    if (!p->reclaim_quiescing) {
        p->reclaim_quiescing     = true;
        p->reclaim_quiesce_since = now;
        return FONT_RECLAIM_QUIESCE;
    }
    if (now - p->reclaim_quiesce_since <= FONT_RECLAIM_QUIESCE_SEC)
        return FONT_RECLAIM_QUIESCE;

    /* Phase C: give up on THIS reclaim rather than on the window.
     *
     * A queue that has not drained in three quarters of a second with nothing new
     * being submitted is not busy — it is a command buffer that will never report
     * completion. Blocking on it would freeze the app; withholding frames forever
     * would freeze the picture. Dropping the request leaves the codepoint that
     * asked for space on its placeholder until the next request comes (the shelf
     * is still full, so one will), which is the only one of the three that keeps
     * the terminal usable. Counted, because it means someone saw a blank glyph. */
    p->reclaim_abandons++;
    atlas_reclaim_clear_request(p);
    return FONT_RECLAIM_NONE;
}

u32 font_atlas_reclaim_abandons(const FontAtlas *atlas) {
    const FontAtlasPriv *p = atlas ? (const FontAtlasPriv *)atlas->priv : NULL;
    return p ? p->reclaim_abandons : 0;
}

void font_atlas_set_gpu_flush(FontAtlas *atlas, void (*fn)(void *)) {
    FontAtlasPriv *p = fa_priv(atlas);
    if (!p) return;
    p->gpu_flush = fn;              /* shares gpu_sync_ctx — same Renderer */
}

void font_atlas_set_gpu_sync(FontAtlas *atlas, bool (*fn)(void *), void *ctx) {
    FontAtlasPriv *p = atlas ? fa_priv(atlas) : NULL;
    if (!p) return;
    p->gpu_sync = fn;
    p->gpu_sync_ctx = ctx;
}

void font_atlas_set_gpu_idle(FontAtlas *atlas, bool (*fn)(void *)) {
    FontAtlasPriv *p = atlas ? fa_priv(atlas) : NULL;
    if (!p) return;               /* shares gpu_sync_ctx — same Renderer */
    p->gpu_idle = fn;
}

u32 font_atlas_layout_version(const FontAtlas *atlas) {
    const FontAtlasPriv *p = atlas ? (const FontAtlasPriv *)atlas->priv : NULL;
    return p ? p->placement_generation : 0;
}
u32 font_atlas_placement_generation(const FontAtlas *atlas) {
    const FontAtlasPriv *p = atlas ? (const FontAtlasPriv *)atlas->priv : NULL;
    return p ? p->placement_generation : 0;
}
u32 font_atlas_content_version(const FontAtlas *atlas) {
    const FontAtlasPriv *p = atlas ? (const FontAtlasPriv *)atlas->priv : NULL;
    return p ? p->content_version : 0;
}
u32 font_atlas_extent_version(const FontAtlas *atlas) {
    const FontAtlasPriv *p = atlas ? (const FontAtlasPriv *)atlas->priv : NULL;
    return p ? p->extent_version : 0;
}
void font_atlas_set_gpu_ready(FontAtlas *atlas, u32 content_version) {
    if (!atlas) return;
    /* Monotonic: a backend that prepared version N cannot un-prepare it, and a
     * stale report (a frame prepared before a newer one) must not roll back. */
    if (content_version > atlas->gpu_ready_version)
        atlas->gpu_ready_version = content_version;
}
u32 font_atlas_gpu_ready_version(const FontAtlas *atlas) {
    return atlas ? atlas->gpu_ready_version : 0;
}
bool font_atlas_upload_pending(const FontAtlas *atlas) {
    const FontAtlasPriv *p = atlas ? (const FontAtlasPriv *)atlas->priv : NULL;
    if (!p || !atlas->gpu_gated) return false;
    return p->content_version > atlas->gpu_ready_version;
}

/* The slot index is the LOW bits of this, and a multiplicative hash only lets
 * the low bits of the key reach the low bits of the product. Plain codepoints
 * vary there, but a sized key (see sized_cache_key) carries its width in bits
 * 21-30: without the fold every size of one glyph would hash to the SAME slot
 * and queue in one 32-entry probe chain. */
static u32 glyph_hash(u32 cp) {
    u32 h = cp * 2654435761u;
    return h ^ (h >> 16);
}

/* Start with the actual working set; grow before applying the bounded
 * eviction policy. All cache mutation remains on the atlas-owning thread. */
static bool cache_resize(FontAtlasPriv *p,u32 capacity) {
    if(capacity>GLYPH_CACHE_SIZE)return false;
    CachedGlyph *next=calloc(capacity,sizeof(*next));if(!next)return false;
    for(u32 i=0;i<p->cache_cap;i++)if(p->cache[i].valid) {
        u32 hash=glyph_hash(p->cache[i].codepoint);bool placed=false;
        for(u32 probe=0;probe<32;probe++) {
            u32 index=(hash+probe)&(capacity-1);
            if(!next[index].valid){next[index]=p->cache[i];placed=true;break;}
        }
        if(!placed){free(next);return false;}
    }
    mem_budget_adjust(MEM_FONT_CPU,(isize)((usize)capacity*sizeof(*next))-(isize)((usize)p->cache_cap*sizeof(*next)));
    free(p->cache);p->cache=next;p->cache_cap=capacity;return true;
}

static CachedGlyph *cache_lookup(FontAtlasPriv *p, u32 cp) {
    u32 idx = glyph_hash(cp);
    for (u32 i = 0; i < 32; i++) { /* linear probe — longer chain for multi-script */
        u32 slot = (idx + i) & (p->cache_cap - 1);
        if (p->cache[slot].valid && p->cache[slot].codepoint == cp) {
            p->cache[slot].last_used_frame = p->glyph_frame;
            return &p->cache[slot];
        }
        if (!p->cache[slot].valid)
            return NULL;
    }
    return NULL;
}

/* Eviction: when the table is hot the coldest ~20% of entries are evicted.
 * Holes cannot be left mid-chain (lookup stops on the first empty slot), so
 * the survivors are snapshot, the table is zeroed, and they are reinserted —
 * full rehash. This is O(N) but only fires once per saturation event, and N is
 * GLYPH_CACHE_SIZE. */
#define GLYPH_CACHE_HIGH_WATER (GLYPH_CACHE_SIZE * 4 / 5)   /* 6553 / 80% */
#define GLYPH_CACHE_EVICT_FRAC 5                            /* drop 1/5th */

/* Drop every cached glyph, keeping the table's invariants (a lookup stops at
 * the first empty slot, so the table cannot simply have holes punched in it).
 * The ASCII / box-drawing pre-warm is NOT in this table — it is addressed
 * through atlas->glyphs[] — so it survives untouched, which is what makes a
 * shelf rewind to the post-pre-warm point safe. */
static void cache_drop_dynamic(FontAtlasPriv *p) {
    if (!p) return;
    memset(p->cache, 0, p->cache_cap*sizeof(*p->cache));
    p->cache_used = 0;
    if(p->lig)memset(p->lig, 0, p->lig_cap*sizeof(*p->lig));
    p->lig_used=0;
}

static void cache_evict_cold(FontAtlasPriv *p) {
    /* Single-threaded (main thread only — invoked from cache_insert via the
     * frame-drain path). Static scratch buffers avoid ~80 KiB of stack; they
     * stay process-wide even though the tables are per-atlas because the call
     * never overlaps itself — every caller is on the main thread. */
    static u64 stamps[GLYPH_CACHE_SIZE];
    static CachedGlyph survivors[GLYPH_CACHE_SIZE];

    u32 n = 0;
    for (u32 i = 0; i < p->cache_cap; i++) {
        if (p->cache[i].valid) stamps[n++] = p->cache[i].last_used_frame;
    }
    if (n == 0) { p->cache_used = 0; return; }

    /* Shell sort to find the kth smallest last_used_frame. n is bounded at
     * 2048 and this fires only at saturation, so cost is negligible. No
     * stdlib qsort dep — keeps the hot file lean. */
    for (u32 gap = n / 2; gap > 0; gap /= 2) {
        for (u32 i = gap; i < n; i++) {
            u64 t = stamps[i];
            u32 j = i;
            while (j >= gap && stamps[j - gap] > t) {
                stamps[j] = stamps[j - gap];
                j -= gap;
            }
            stamps[j] = t;
        }
    }
    u32 k = n / GLYPH_CACHE_EVICT_FRAC;
    if (k == 0) k = 1;
    if (k >= n) k = n - 1;
    u64 cutoff = stamps[k]; /* entries with last_used_frame <= cutoff are cold */

    /* Snapshot survivors, then rehash. The snapshot must come first because
     * the table is wiped next. */
    u32 survivor_count = 0;
    u32 evicted = 0;
    for (u32 i = 0; i < p->cache_cap; i++) {
        if (!p->cache[i].valid) continue;
        if (p->cache[i].last_used_frame <= cutoff && evicted < k) {
            evicted++;
            continue;
        }
        survivors[survivor_count++] = p->cache[i];
    }

    memset(p->cache, 0, p->cache_cap*sizeof(*p->cache));
    p->cache_used = 0;

    /* Reinsert survivors via the standard probe path so lookup invariants
     * hold. */
    for (u32 s = 0; s < survivor_count; s++) {
        u32 idx = glyph_hash(survivors[s].codepoint);
        for (u32 i = 0; i < 32; i++) {
            u32 slot = (idx + i) & (p->cache_cap - 1);
            if (!p->cache[slot].valid) {
                p->cache[slot] = survivors[s];
                p->cache_used++;
                break;
            }
        }
        /* A survivor that cannot find a slot within the probe window is
         * dropped; it is re-rasterized on demand. */
    }
}

/* Shared 32-slot linear probe for both insert paths. Returns the slot to
 * write: the slot already holding `cp` (caller updates in place — cache_used
 * must NOT grow), else the first invalid slot in the window (fresh insert —
 * caller bumps cache_used), else — window full, all 32 slots valid with
 * other codepoints — the first slot of the chain to overwrite (one valid
 * entry replaces another; cache_used unchanged). Callers distinguish the
 * three cases by inspecting p->cache[slot] before writing. */
static u32 cache_probe_slot(FontAtlasPriv *p, u32 idx, u32 cp) {
    for (u32 i = 0; i < 32; i++) {
        u32 slot = (idx + i) & (p->cache_cap - 1);
        if (p->cache[slot].valid && p->cache[slot].codepoint == cp) return slot;
        if (!p->cache[slot].valid) return slot;
    }
    if(p->cache_cap<GLYPH_CACHE_SIZE && cache_resize(p,p->cache_cap*2))
        return cache_probe_slot(p,idx,cp);
    return idx & (p->cache_cap - 1);
}

/* Insert a negative/"atlas-exhausted" sentinel for `cp`: the codepoint could
 * not be packed (atlas hit ATLAS_MAX_SIZE). The cached entry's UVs point at
 * the space glyph and its `exhausted` flag is set, so font_get_glyph_rect
 * returns the space placeholder on a cache HIT instead of re-running the full
 * rasteriser every single frame. The entry participates in normal LRU/eviction
 * and is wiped on font reload, so if the atlas is later reset/regrown the
 * codepoint gets a fresh real attempt. */
static void cache_insert_exhausted(FontAtlas *atlas, u32 cp) {
    FontAtlasPriv *p = fa_priv(atlas);
    if (!p) return;
    if (p->cache_used >= p->cache_cap*4/5 && p->cache_cap<GLYPH_CACHE_SIZE)
        (void)cache_resize(p,p->cache_cap*2);
    if (p->cache_used >= p->cache_cap*4/5) cache_evict_cold(p);
    f32 su0 = atlas->glyphs[32].ax0, sv0 = atlas->glyphs[32].ay0;
    f32 su1 = atlas->glyphs[32].ax1, sv1 = atlas->glyphs[32].ay1;
    u32 slot = cache_probe_slot(p, glyph_hash(cp), cp);
    if (p->cache[slot].valid && p->cache[slot].codepoint == cp) {
        /* Already present (real or sentinel) — mark/refresh in place. */
        p->cache[slot].exhausted = true;
        p->cache[slot].x0 = su0; p->cache[slot].y0 = sv0;
        p->cache[slot].x1 = su1; p->cache[slot].y1 = sv1;
        p->cache[slot].kind = atlas->metal_seed_texture ? FONT_GLYPH_SEED : FONT_GLYPH_ALPHA;
        p->cache[slot].content_version = 0;
        p->cache[slot].last_used_frame = p->glyph_frame;
        return;
    }
    /* Fresh insert into an empty slot grows the live count; the window-full
     * fallback overwrites a valid entry (same policy as cache_insert) and
     * leaves cache_used unchanged. */
    bool was_empty = !p->cache[slot].valid;
    p->cache[slot] = (CachedGlyph){ .codepoint = cp, .x0 = su0, .y0 = sv0,
                                    .x1 = su1, .y1 = sv1,
                                    .last_used_frame = p->glyph_frame,
                                    .content_version = 0,
                                    .kind = atlas->metal_seed_texture ? FONT_GLYPH_SEED : FONT_GLYPH_ALPHA,
                                    .valid = true, .exhausted = true };
    if (was_empty) p->cache_used++;
}

/* How many glyphs this process has PACKED into an alpha atlas, monotonic and
 * never reset — a reader keeps its own snapshot, the way the GPU-timing and
 * texture-upload counters do.
 *
 * A cache_insert is the one chokepoint every rasterised glyph passes through,
 * from both the worker drain and the synchronous path, so the difference
 * between two snapshots is exactly "how much rasterising did that cost". Asked
 * of a working set larger than GLYPH_CACHE_HIGH_WATER it is what makes eviction
 * churn measurable instead of inferred. */
static _Atomic u64 g_font_glyphs_packed = 0;

u64 font_glyphs_packed(void) {
    return atomic_load_explicit(&g_font_glyphs_packed, memory_order_relaxed);
}

static CachedGlyph *cache_insert(FontAtlasPriv *p, u32 cp, f32 x0, f32 y0, f32 x1, f32 y1, u8 kind) {
    atomic_fetch_add_explicit(&g_font_glyphs_packed, 1, memory_order_relaxed);
    if (p->cache_used >= p->cache_cap*4/5 && p->cache_cap<GLYPH_CACHE_SIZE)
        (void)cache_resize(p,p->cache_cap*2);
    if (p->cache_used >= p->cache_cap*4/5) cache_evict_cold(p);

    u32 slot = cache_probe_slot(p, glyph_hash(cp), cp);
    /* Only a fresh insert into an empty slot grows the live count. The other
     * two probe outcomes replace like-for-like: updating an existing entry in
     * place (e.g. an exhausted sentinel being upgraded to a real glyph, or a
     * redundant insert), or — probe window exhausted, rare — overwriting the
     * first slot of the chain, which is guaranteed valid there (all 32 probed
     * slots were valid, else the probe would have returned an empty one).
     * Incrementing cache_used in either case would over-count a single
     * codepoint across two valid slots and trip cache_evict_cold prematurely. */
    bool was_empty = !p->cache[slot].valid;
    p->cache[slot] = (CachedGlyph){ .codepoint = cp, .x0 = x0, .y0 = y0,
                                    .x1 = x1, .y1 = y1,
                                    .last_used_frame = p->glyph_frame,
                                    .content_version = p->content_version,
                                    .kind = kind,
                                    .valid = true };
    if (was_empty) p->cache_used++;
    return &p->cache[slot];
}

/* LIU_SPRITE=0 falls back to the font for every procedurally drawn range. */
static bool sprite_enabled(void) {
    static i32 v = -1;
    if (v < 0) { const char *e = getenv("LIU_SPRITE"); v = (e && atoi(e) == 0) ? 0 : 1; }
    return v != 0;
}

/* Rasterize a glyph on demand — searches primary + fallback fonts */
#if LIU_ANIM_PROBE
/* Probe instrumentation: glyphs rasterised into the atlas. A frame that has to
 * build glyphs is doing work no later frame repeats, which is exactly the shape
 * of "the animation stutters, but only the first time". */
i32 g_font_probe_raster = 0;
#endif

/* Rasterise a colour glyph at an ARBITRARY box size and cache it under `key`.
 *
 * Split out of rasterize_glyph so large UI text can have its own raster: the
 * colour atlas otherwise holds one copy per emoji at terminal-cell size, and a
 * Markdown H1 magnified that 17x37 bitmap to twice the size — the emoji went
 * visibly soft next to the crisp headline text beside it. Same reasoning as
 * font_get_glyph_rect_sized for alpha glyphs, same packing state, different
 * cache key. Returns NULL when the colour atlas cannot fit the box. */
/* Ceiling on the colour atlas. 512 -> 1024 -> 2048; at a Retina cell that is
 * roughly 130 -> 530 -> 2100 distinct emoji, and 2100 covers every emoji a
 * session realistically shows. RGBA at 2048 is 16 MB, and only ever paid by a
 * session that actually asked for that many. */
#define COLOR_ATLAS_MAX_SIZE 2048

/* Drop every colour-atlas entry from the glyph cache, keeping the alpha ones.
 *
 * The table is open-addressed with no tombstones, so lookup stops at the first
 * empty slot — individual entries cannot just be cleared, the table has to be
 * rebuilt. Same shape as cache_evict_cold's rehash, different predicate: a
 * colour entry is the one with a NEGATIVE u0 (font.c's marker for "this lives
 * in the colour atlas"), and `exhausted` sentinels go too — they are precisely
 * the glyphs that were refused for lack of room and now have some. */
static void cache_drop_color(FontAtlasPriv *p) {
    static CachedGlyph survivors[GLYPH_CACHE_SIZE];
    u32 n = 0;
    for (u32 i = 0; i < p->cache_cap; i++) {
        if (!p->cache[i].valid) continue;
        if (p->cache[i].kind == FONT_GLYPH_COLOR || p->cache[i].exhausted) continue;
        survivors[n++] = p->cache[i];
    }
    memset(p->cache, 0, p->cache_cap*sizeof(*p->cache));
    p->cache_used = 0;
    for (u32 s = 0; s < n; s++) {
        u32 idx = glyph_hash(survivors[s].codepoint);
        for (u32 i = 0; i < 32; i++) {
            u32 slot = (idx + i) & (p->cache_cap - 1);
            if (!p->cache[slot].valid) {
                p->cache[slot] = survivors[s];
                p->cache_used++;
                break;
            }
        }
    }
}

/* Double the colour atlas and start its packer over. Returns false when it is
 * already at COLOR_ATLAS_MAX_SIZE, or the new storage could not be had.
 *
 * The alpha atlas grows and KEEPS its contents, because its CPU bitmap is the
 * backing store and a realloc plus a row copy preserves every packed glyph.
 * This one's backing store is a Metal texture whose CPU mirror is optional, so
 * rather than blit texture-to-texture it resets the shelf and drops the colour
 * cache; the emoji still on screen re-rasterise on demand.
 *
 * That re-raster is a real cost — the colour path is synchronous CoreText —
 * but it is bounded by what is visible, and it happens at most twice in the
 * life of an atlas. Without growth, every new emoji past the ceiling would be
 * invisible for good. */
static bool color_atlas_grow(FontAtlas *atlas) {
    FontAtlasPriv *p = fa_priv(atlas);
    if (!atlas || !p) return false;
    if (atlas->color_atlas_w <= 0) return false;
    if (atlas->color_atlas_w >= COLOR_ATLAS_MAX_SIZE ||
        atlas->color_atlas_h >= COLOR_ATLAS_MAX_SIZE) return false;

    const i32 nw = atlas->color_atlas_w * 2;
    const i32 nh = atlas->color_atlas_h * 2;

    /* Allocate the replacement mirror BEFORE tearing anything down, so a
     * failure here leaves the old (smaller but working) atlas intact. */
    u8 *nbmp = NULL;
    if (atlas->color_bitmap) {
        nbmp = calloc(1, (usize)nw * (usize)nh * 4u);
        if (!nbmp) return false;
    }

    /* Same rule as the alpha grow, and this one is harsher: the shelf resets
     * to (0,0) and the colour cache is dropped, so every emoji UV emitted
     * earlier this frame names a rectangle about to hold something else.
     * Publish those instances against the atlas they were measured in. Placed
     * after the allocation above so a failure path still flushes nothing it
     * did not need to. */
    if (p->gpu_flush) p->gpu_flush(p->gpu_sync_ctx);

#ifdef USE_METAL
    if (atlas->color_texture) {
        /* Matches the +1 from newTextureWithDescriptor: and the explicit +1
         * retain in font_ensure_color_atlas_metal(); the next rasterise
         * recreates it at the new size. The renderer holds its own reference
         * to whatever it has bound, so the frame in flight keeps its texture;
         * the swap to the replacement is published at the next boundary. */
        id ctex = (__bridge id)atlas->color_texture;
        if (atlas->gpu_uploader)
            renderer_atlas_texture_replaced(atlas->gpu_uploader, FONT_GLYPH_COLOR,
                                            (__bridge void *)ctex, NULL, 0, 0);
        [ctex release];
        [ctex release];
        atlas->color_texture = NULL;
    }
#else
    if (atlas->color_texture_gl) {
        /* Same contract as the Metal arm: drop it and let the next rasterise
         * recreate it at the new size. */
        GLuint ctex = (GLuint)atlas->color_texture_gl;
        glDeleteTextures(1, &ctex);
        atlas->color_texture_gl = 0;
    }
#endif
    free(atlas->color_bitmap);
    atlas->color_bitmap = nbmp;

    atlas->color_atlas_w = nw;
    atlas->color_atlas_h = nh;
    atlas->color_atlas_x = 0;
    atlas->color_atlas_y = 0;
    atlas->color_row_h   = 0;
    cache_drop_color(p);
    /* The COLOUR atlas does not copy its pixels: it resets the shelf to (0,0)
     * and drops the colour cache, so every emoji rect recorded before now names
     * a rectangle that is about to hold something else — a PLACEMENT change,
     * unlike the alpha grow. Row caches hold those rects baked into GPU
     * instances; the generation is what rebuilds them. */
    p->placement_generation++;
    p->extent_version++;
    font_atlas_reaccount(atlas);

    fprintf(stderr, "font: color atlas grew to %dx%d\n", nw, nh);
    return true;
}

/* Fill an `ecw` x `ch` RGBA box with the colour glyph for `cp`.
 *
 * The only part of the colour path that is not portable. macOS shapes and
 * draws through Core Text; Linux has no shaper here, so it takes the bitmap
 * the CBDT font stores for the codepoint and box-fits it. Everything around
 * this — packing, growth, the exhausted sentinel, the cache key — is shared,
 * so both platforms pack and grow the colour atlas the same way. */
static bool color_glyph_fill_rgba_payload(u8 *rgba, i32 ecw, i32 ch, u32 cp,
                                          const u8 *text, u32 len) {
#ifdef PLATFORM_MACOS
    (void)cp;
    return ct_rasterize_color_text(rgba, ecw, ch, text, len, g_font_ascent);
#elif defined(PLATFORM_LINUX)
    (void)text; (void)len;
    if (!emoji_source()) return false;
    i32 sw = 0, sh = 0;
    u8 *src = emoji_font_glyph_rgba(g_emoji_font, cp, &sw, &sh);
    if (!src) return false;
    if (sw <= 0 || sh <= 0) { free(src); return false; }

    /* Fit the square source inside the box without distorting it, then centre
     * the leftover. */
    i32 dw = ecw, dh = ch;
    if ((i64)sw * dh > (i64)sh * dw) dh = (i32)((i64)sh * dw / sw);
    else                             dw = (i32)((i64)sw * dh / sh);
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    i32 ox = (ecw - dw) / 2, oy = (ch - dh) / 2;

    u8 *scaled = calloc((usize)dw * (usize)dh, 4);
    if (!scaled) { free(src); return false; }
    emoji_resample(src, sw, sh, scaled, dw, dh);
    for (i32 row = 0; row < dh; row++)
        memcpy(rgba + (((usize)(oy + row) * (usize)ecw) + (usize)ox) * 4,
               scaled + (usize)row * (usize)dw * 4, (usize)dw * 4);
    free(scaled);
    free(src);
    return true;
#else
    (void)rgba; (void)ecw; (void)ch; (void)cp; (void)text; (void)len;
    return false;
#endif
}

static bool color_glyph_fill_rgba(u8 *rgba, i32 w, i32 h, u32 cp) {
    u8 text[UTF8_CLUSTER_MAX * 4];
    u32 len = utf8_cluster_encode(cp, text, sizeof text);
    return color_glyph_fill_rgba_payload(rgba, w, h, cp, text, len);
}

/* Main-thread-only packing for an already rasterized RGBA cell. The worker
 * supplies pixels; texture creation, shelf growth and upload stay here. */
static CachedGlyph *color_bitmap_install(FontAtlas *atlas, u32 key,
                                          const u8 *rgba, i32 ecw, i32 ch) {
    FontAtlasPriv *p = fa_priv(atlas);
    if (!p || atlas->color_atlas_w <= 0 || ecw <= 0 || ch <= 0) return NULL;
    while (ecw + 2 > atlas->color_atlas_w) {
        if (!color_atlas_grow(atlas)) { cache_insert_exhausted(atlas, key); return NULL; }
    }
#ifdef USE_METAL
    if (!atlas->color_texture) (void)font_ensure_color_atlas_metal(atlas);
#else
    if (!atlas->color_texture_gl && atlas->texture_id)
        (void)font_ensure_color_atlas_gl(atlas);
#endif
    for (;;) {
        if (atlas->color_atlas_x + ecw + 2 > atlas->color_atlas_w) {
            atlas->color_atlas_x = 0;
            atlas->color_atlas_y += atlas->color_row_h + 2;
            atlas->color_row_h = 0;
        }
        if (atlas->color_atlas_y + ch + 2 <= atlas->color_atlas_h) break;
        if (!color_atlas_grow(atlas)) { cache_insert_exhausted(atlas, key); return NULL; }
    }
    i32 ax = atlas->color_atlas_x + 1, ay = atlas->color_atlas_y + 1;
    if (!font_gpu_upload(atlas, atlas->color_texture, atlas->color_texture_gl, GL_RGBA,
                         ax, ay, ecw, ch, rgba, (usize)ecw * (usize)ch * 4u)) {
        p->served_placeholder = true;
        return NULL;
    }
    if (atlas->color_bitmap) {
        for (i32 row = 0; row < ch; row++)
            memcpy(atlas->color_bitmap + (ay + row) * atlas->color_atlas_w * 4 + ax * 4,
                   rgba + row * ecw * 4, (usize)(ecw * 4));
    }
    atlas->color_atlas_x += ecw + 2;
    if (ch + 2 > atlas->color_row_h) atlas->color_row_h = ch + 2;
    return cache_insert(p, key, (f32)ax, (f32)ay, (f32)(ax + ecw), (f32)(ay + ch),
                        FONT_GLYPH_COLOR);
}

static CachedGlyph *rasterize_color_boxed(FontAtlas *atlas, u32 cp, u32 key,
                                          i32 ecw, i32 ch) {
    if (!fa_priv(atlas) || atlas->color_atlas_w <= 0 || ecw <= 0 || ch <= 0) return NULL;
    usize pixels, bytes;
    if (!checked_mul_usize((usize)ecw, (usize)ch, &pixels) ||
        !checked_mul_usize(pixels, 4u, &bytes)) return NULL;
    u8 *rgba = calloc(1, bytes);
    if (!rgba) return NULL;
    CachedGlyph *result = color_glyph_fill_rgba(rgba, ecw, ch, cp)
        ? color_bitmap_install(atlas, key, rgba, ecw, ch) : NULL;
    free(rgba);
    return result;
}

/* Rasterise one glyph into `atlas`. The caller holds g_face_mu with the face
 * already bound to this atlas — see rasterize_glyph() below. */
static CachedGlyph *rasterize_glyph_locked(FontAtlas *atlas, u32 cp) {
#if LIU_ANIM_PROBE
    g_font_probe_raster++;
#endif
    if (!g_font_data || g_font_scale == 0) return NULL;
    FontAtlasPriv *p = fa_priv(atlas);
    if (!p) return NULL;

    /* WHAT THIS GLYPH IS FILED UNDER, as opposed to what gets drawn.
     *
     * They are the same thing for everything except a cluster the shaper
     * declines: the fall-through below replaces `cp` with the cluster's BASE
     * so something legible is still drawn, but the entry is filed under the
     * cluster HANDLE. Keyed on the replacement, `cache_lookup` would miss the
     * handle forever, and since clusters are excluded from the async path
     * each lookup would re-run the full rasteriser and take a fresh atlas
     * slot, per cell per row rebuild. On the GL backend there is no shaper,
     * so EVERY cluster takes the fall-through. */
    const u32 key = cp;
    /* A bold or italic glyph is filed under its codepoint plus the style
     * bits; everything below draws the codepoint in that style. */
    const u32 style = cp & FONT_STYLE_MASK;
    const bool icon_wide = (cp & FONT_ICON_WIDE) != 0;
    cp &= ~FONT_KEY_FLAGS;

    /* Double-width glyphs (CJK, fullwidth forms) get a two-cell raster. The
     * grid already reserves two columns for them — rasterising into one and
     * drawing it in one leaves a gap after every ideograph, which is what made
     * 漢字 read as 漢 字. Emoji reach the two-cell box through the colour path
     * below instead. */
    i32 cw = (i32)atlas->cell_width;
    i32 ch = (i32)atlas->cell_height;
    if (font_glyph_is_wide(cp) || icon_wide) cw *= 2;

    /* Colour emoji: rasterise into the RGBA colour atlas. The backing texture
     * is created lazily on the very first emoji the process encounters, which
     * saves ~1 MB of GPU memory for users who never type one. */
    if (utf8_is_emoji(cp) && atlas->color_atlas_w > 0) {
        /* Two cells, or one for an emoji the grid gives one column (a
         * text-default emoji without VS16): scaled into the cell. */
        CachedGlyph *cg = rasterize_color_boxed(atlas, cp, key,
                                                cw * (utf8_char_width(cp) >= 2 ? 2 : 1), ch);
        if (cg) return cg;
        /* Fall through to the alpha paths below (cluster / box drawing / face
         * glyph) — an emoji the colour atlas could not take still draws. */
    }

#ifdef PLATFORM_MACOS
    /* Non-emoji cluster (letter + combining marks, Indic conjunct): shaped
     * into the ordinary single-cell alpha atlas. Core Text only — there is no
     * shaper on the Linux path, where a cluster falls back to its base glyph.
     *
     * This is the ONLY composite path: a cell holds one u32, so a base plus
     * its marks arrives here as an interned CLUSTER HANDLE, never as an array.
     */
    if (utf8_is_cluster(cp)) {
        u8 *alpha = calloc((usize)cw, (usize)ch);
        if (alpha) {
            if (ct_rasterize_cluster_alpha(alpha, cw, ch, cp, g_font_ascent, style) &&
                font_ensure_atlas_room(atlas, cw, ch)) {
                i32 ax = p->pack_x + 1, ay = p->pack_y + 1;
                atlas_mirror_r8(atlas, ax, ay, cw, ch, alpha);
                if (!font_gpu_upload(atlas, atlas->metal_texture, atlas->texture_id, GL_RED,
                                     ax, ay, cw, ch, alpha, (usize)cw * (usize)ch)) {
                    free(alpha);
                    p->served_placeholder = true;
                    return NULL;
                }
                free(alpha);
                p->pack_x += cw + 2;
                if (ch + 2 > p->pack_row_h) p->pack_row_h = ch + 2;
                p->packed_since_reset++;
                return cache_insert(p, key, (f32)ax, (f32)ay, (f32)(ax + cw), (f32)(ay + ch),
                                    FONT_GLYPH_ALPHA);
            }
            free(alpha);
        }
    }
#endif
    /* Without CoreText a cluster has no shaper: draw its base codepoint. Also
     * reached on macOS when the shaping above failed or found no atlas room.
     * `key` keeps the handle, so the result is still filed under what the
     * caller asked for and this runs once rather than once per lookup. */
    if (utf8_is_cluster(cp)) cp = utf8_cluster_base(cp);

    /* Procedurally drawn ranges: box drawing, block elements, braille and the
     * powerline separators. Pixel-perfect from the cell geometry.
     *
     * This is the SECOND dispatch for these ranges — rasterize_alpha_bitmap_m
     * has the other one, on the async worker's path. Both must list the same
     * codepoints, which is what sprite_is_procedural() is for: a range added
     * to only one of them renders correctly exactly when it happens to be
     * pre-warmed and blank the rest of the time. */
    if (sprite_is_procedural(cp)) {
        u8 *alpha = calloc((usize)cw, (usize)ch);
        if (!alpha) return NULL;
        if (sprite_draw(cp, alpha, cw, ch, sprite_line(atlas))) {
            if (!font_ensure_atlas_room(atlas, cw, ch)) { free(alpha); return NULL; }
            i32 ax = p->pack_x + 1, ay = p->pack_y + 1;
            atlas_mirror_r8(atlas, ax, ay, cw, ch, alpha);
            if (!font_gpu_upload(atlas, atlas->metal_texture, atlas->texture_id, GL_RED,
                                 ax, ay, cw, ch, alpha, (usize)cw * (usize)ch)) {
                free(alpha);
                p->served_placeholder = true;
                return NULL;
            }
            free(alpha);
            f32 u0 = (f32)ax, v0 = (f32)ay, u1 = (f32)(ax + cw), v1 = (f32)(ay + ch);
            p->pack_x += cw + 2;
            if (ch + 2 > p->pack_row_h) p->pack_row_h = ch + 2;
            p->packed_since_reset++;
            if (key == cp && cp < 256) { atlas->glyphs[cp].ax0=u0; atlas->glyphs[cp].ay0=v0; atlas->glyphs[cp].ax1=u1; atlas->glyphs[cp].ay1=v1; }
            return cache_insert(p, key, u0, v0, u1, v1, FONT_GLYPH_ALPHA);
        }
        free(alpha);
        /* Fall through to font-based rendering */
    }

    if (!font_ensure_atlas_room(atlas, cw, ch)) {
        /* Atlas is full and cannot grow (hit ATLAS_MAX_SIZE). Reclaim the area
         * on-demand glyphs took and try once more — without this the sentinel
         * below would be permanent and every later codepoint would draw as a
         * blank space for the life of the atlas. */
        if (!atlas_reclaim_shelf(atlas) ||
            !font_ensure_atlas_room(atlas, cw, ch)) {
            /* Reclamation is refused when it would thrash (nothing was packed
             * since the last one), so record a sentinel: the codepoint renders
             * as a space and is NOT re-rasterised on every subsequent frame,
             * which would be a sustained CoreText cliff. */
            cache_insert_exhausted(atlas, key);
            return NULL;
        }
    }

    /* Rasterize into R8 alpha temp buffer. */
    u8 *tmp = calloc((usize)cw, (usize)ch);
    if (!tmp) return NULL;

    /* THE SAME LADDER the worker's raster runs — Core Text, then FreeType,
     * then stb_truetype — rather than a second copy of it, so the synchronous
     * and async paths cannot disagree. The caller holds g_face_mu with the
     * face bound to this atlas, which is that function's precondition.
     *
     * The one thing this path adds is the SPACE ALIAS: when nothing on the
     * machine covers the codepoint, file an entry pointing at the space glyph
     * so font_get_glyph_rect stops asking for it every frame. ' ' itself is
     * exempt — a face with no space glyph still gets a blank cell of its own,
     * which is what atlas->glyphs[32] and every placeholder rest on. */
    bool rasterized = rasterize_alpha_bitmap_face_locked(cp, tmp, cw, ch, 1.0f, style);
    if (!rasterized && cp != ' ') {
        free(tmp);
        CachedGlyph *space = cache_lookup(p, 32);
        if (space) return cache_insert(p, key, space->x0, space->y0, space->x1, space->y1, space->kind);
        return NULL;
    }

    /* Upload to atlas texture */
    i32 ax = p->pack_x + 1;
    i32 ay = p->pack_y + 1;

    /* cw is the span: a double-width glyph occupies two cells in the atlas,
     * font_glyph_is_wide() having doubled it above. */
    atlas_mirror_r8(atlas, ax, ay, cw, ch, tmp);
    if (!font_gpu_upload(atlas, atlas->metal_texture, atlas->texture_id, GL_RED,
                         ax, ay, cw, ch, tmp, (usize)cw * (usize)ch)) {
        free(tmp);
        p->served_placeholder = true;
        return NULL;
    }
    free(tmp);

    /* Texel rect */
    f32 u0f = (f32)ax, v0f = (f32)ay, u1f = (f32)(ax + cw), v1f = (f32)(ay + ch);

    /* Advance packing cursor */
    p->pack_x += cw + 2;
    if (ch + 2 > p->pack_row_h) p->pack_row_h = ch + 2;
    p->packed_since_reset++;

    /* Store in atlas glyphs array for ASCII fast path.
     *
     * Gated on this raster actually BEING that codepoint. A cluster whose
     * shaping was declined degrades to its base, and a base under 256 would
     * otherwise rewrite atlas->glyphs[base] — including glyphs[32], the space
     * every placeholder and every exhausted sentinel points at — with a UV
     * further down the shelf, where the next reclaim hands the pixels to some
     * other glyph. */
    if (key == cp && cp < 256) {
        atlas->glyphs[cp].ax0 = u0f;
        atlas->glyphs[cp].ay0 = v0f;
        atlas->glyphs[cp].ax1 = u1f;
        atlas->glyphs[cp].ay1 = v1f;
    }

    return cache_insert(p, key, u0f, v0f, u1f, v1f, FONT_GLYPH_ALPHA);
}

/* The main thread's raster entry. It holds the face for the duration rather
 * than binding and letting go: another window's raster worker binds the face
 * to ITS atlas under the same lock, and between a bind and the glyph the face
 * would otherwise have moved again. font_atlas_create already holds the lock
 * when it pre-warms, so that path calls the _locked form directly. */
static CachedGlyph *rasterize_glyph(FontAtlas *atlas, u32 cp) {
    pthread_mutex_lock(&g_face_mu);
    CachedGlyph *g = face_bind_locked(atlas) ? rasterize_glyph_locked(atlas, cp) : NULL;
    pthread_mutex_unlock(&g_face_mu);
    return g;
}

/* =========================================================================
 * Public: get glyph UV (with on-demand rasterization)
 * ========================================================================= */

static inline void rect_from_ascii(const FontAtlas *atlas, u32 cp, FontGlyphRect *out) {
    out->x0 = atlas->glyphs[cp].ax0;
    out->y0 = atlas->glyphs[cp].ay0;
    out->x1 = atlas->glyphs[cp].ax1;
    out->y1 = atlas->glyphs[cp].ay1;
    out->kind = atlas->metal_seed_texture ? FONT_GLYPH_SEED : FONT_GLYPH_ALPHA;
}
static inline void rect_from_cached(const CachedGlyph *c, FontGlyphRect *out) {
    out->x0 = c->x0; out->y0 = c->y0; out->x1 = c->x1; out->y1 = c->y1;
    out->kind = c->kind;
}
/* A glyph whose texels the GPU has not been handed yet is drawn as the space
 * placeholder and its row is marked pending, exactly as for a glyph the async
 * worker is still rasterising: the frame after the upload is prepared repairs
 * the row. Returns true when the placeholder was served. */
static inline bool rect_serve_if_unready(FontAtlas *atlas, FontAtlasPriv *p,
                                         u32 content_version, FontGlyphRect *out) {
    if (glyph_gpu_ready(atlas, content_version)) return false;
    p->served_placeholder = true;
    rect_from_ascii(atlas, 32, out);
    return true;
}

bool font_get_glyph_rect(FontAtlas *atlas, u32 cp, FontGlyphRect *out) {
    /* Fast path: ASCII. The pre-warm seeded these before any frame, so they
     * are ready by the time anything can draw. */
    if (cp >= 32 && cp < 127) {
        rect_from_ascii(atlas, cp, out);
        return true;
    }

    FontAtlasPriv *p = fa_priv(atlas);
    if (!p) goto space_fallback;

    /* Cache lookup */
    CachedGlyph *cached = cache_lookup(p, cp);
    if (cached) {
        /* Atlas-exhausted sentinel: the rect is already the space glyph's.
         * Returning false tells the caller this is a placeholder, while the
         * cache HIT prevents the per-frame re-rasterisation cliff. */
        if (cached->exhausted) { rect_from_cached(cached, out); return false; }
        if (rect_serve_if_unready(atlas, p, cached->content_version, out)) return true;
        rect_from_cached(cached, out);
        return true;
    }

    /* Face glyphs, emoji and clusters enqueue a captured payload and return
     * the space placeholder until the next completion drain installs them. */
    /* Procedurally drawn glyphs stay synchronous,
     * and this is a correctness requirement, not a tuning knob: they never
     * touch the face, so the worker buys nothing, and a placeholder frame for
     * a shape that costs microseconds to draw is a visible flash of a SPACE
     * where a prompt separator or a plotted dot should be. Box drawing looked
     * synchronous before only because atlas creation pre-warms its 160
     * codepoints (see the warm-up loop); braille is 256 codepoints and is
     * deliberately not pre-warmed, so without this it would have gone through
     * the worker and returned a blank on first sight. */
    if (p->raster.started && !sprite_is_procedural(cp)) {
        raster_enqueue(atlas, p, cp);
        p->served_placeholder = true;
        rect_from_ascii(atlas, 32, out);
        return true; /* placeholder is valid */
    }

    /* Synchronous fallback: worker not started, or procedural geometry. The raster
     * lands in the CPU atlas now, but on a backend that prepares uploads at
     * the frame boundary its texels reach the GPU with the NEXT frame — so
     * this lookup, too, hands back the placeholder and marks the row. */
    cached = rasterize_glyph(atlas, cp);
    if (cached) {
        if (rect_serve_if_unready(atlas, p, cached->content_version, out)) return true;
        rect_from_cached(cached, out);
        return true;
    }

space_fallback:
    /* Fallback to space */
    rect_from_ascii(atlas, 32, out);
    return false;
}

/* =========================================================================
 * Sized glyphs — crisp large UI text
 *
 * The atlas holds one raster per codepoint, at the terminal cell size. Drawing
 * a markdown H1 from it means magnifying a 17x37 bitmap to 30x65: the sampler
 * interpolates, and the heading reads blurry next to body text that happens to
 * land near 1:1. The fix is not a bigger atlas (that would soften every
 * terminal cell instead) but a second raster of the same glyph at the size it
 * is actually drawn, packed into the same atlas under a synthetic key.
 *
 * Only the UVs change — quad geometry, shader and batching are untouched, so a
 * heading still costs one instance per glyph in the same draw call.
 * ========================================================================= */

/* Cache key for (glyph, cell width) — LOSSLESS, never a hash.
 *
 *   bit 31      tag: a plain codepoint (< 0x110000) and a cluster handle
 *               (bit 30) can never equal a sized key
 *   bit 30      always clear — a cluster handle's bit, so no sized key ever
 *               passes utf8_is_cluster()
 *   bits 21-29  the bucketed width, 1..511 px
 *   bits 0-20   the glyph: the codepoint, or 0x110000 + the cluster's index
 *
 * Lookup, probe and insert match on this u32 alone, so with a hash of the
 * pair two different glyphs whose hashes agreed would share one entry and the
 * second would draw as the first, at the first one's size. A key that is the
 * pair itself cannot collide.
 *
 * Returns 0 for a pair that does not fit; the caller serves the shared,
 * cell-sized copy instead. 0 is never a valid sized key (the tag bit is set). */
#define SIZED_KEY_TAG       0x80000000u
#define SIZED_KEY_CP_BITS   21u
#define SIZED_KEY_W_SHIFT   SIZED_KEY_CP_BITS
#define SIZED_KEY_W_MAX     511
#define SIZED_KEY_CLUSTER0  0x110000u          /* first value above Unicode */

static u32 sized_cache_key(u32 cp, i32 cell_w) {
    if (cell_w <= 0 || cell_w > SIZED_KEY_W_MAX) return 0;
    u32 g;
    if (utf8_is_cluster(cp)) g = SIZED_KEY_CLUSTER0 + (cp & ~UTF8_CLUSTER_BIT);
    else if (cp < SIZED_KEY_CLUSTER0) g = cp;
    else return 0;
    if (g >= (1u << SIZED_KEY_CP_BITS)) return 0;
    return SIZED_KEY_TAG | ((u32)cell_w << SIZED_KEY_W_SHIFT) | g;
}

/* Size buckets keep the number of variants bounded: a caller animating a scale
 * would otherwise rasterise a fresh set every frame and exhaust the atlas. */
#define SIZED_BUCKET_PX   2
#define SIZED_MAX_SCALE   4.0f
/* Below this the atlas cell is close enough to 1:1 that a second raster buys
 * nothing — and body text must keep using the shared cell so the terminal and
 * the reader stay pixel-identical. */
#define SIZED_MIN_SCALE   1.12f
#ifdef PLATFORM_LINUX
/* Mirror of the window above, for the shrink direction. Core Text hints a
 * glyph into whatever box it is given, so macOS never needed this; the stb
 * path rasterises once at the terminal cell and lets the sampler do the rest,
 * which at 1x turns 10px stems into 8px mush. Below SHRINK_MIN there is too
 * little ink left for a dedicated raster to beat the resampler. */
#define SIZED_SHRINK_MAX_SCALE 0.89f
#define SIZED_SHRINK_MIN_SCALE 0.35f
#endif

bool font_get_glyph_rect_sized(FontAtlas *atlas, u32 cp, i32 cell_w,
                               FontGlyphRect *out) {
    FontAtlasPriv *p = fa_priv(atlas);
    if (!atlas || !p || atlas->cell_width <= 0.0f || atlas->cell_height <= 0.0f ||
        cell_w <= 0)
        return font_get_glyph_rect(atlas, cp, out);

    f32 scale = (f32)cell_w / atlas->cell_width;
    bool worth_it = scale >= SIZED_MIN_SCALE;
#ifdef PLATFORM_LINUX
    worth_it = worth_it || (scale <= SIZED_SHRINK_MAX_SCALE &&
                            scale >= SIZED_SHRINK_MIN_SCALE);
#endif
    if (!worth_it)
        return font_get_glyph_rect(atlas, cp, out);
    if (scale > SIZED_MAX_SCALE) {
        scale = SIZED_MAX_SCALE;
        cell_w = (i32)roundf(atlas->cell_width * scale);
    }
    i32 bw = ((cell_w + SIZED_BUCKET_PX - 1) / SIZED_BUCKET_PX) * SIZED_BUCKET_PX;

#if defined(PLATFORM_MACOS) || defined(PLATFORM_LINUX)
    /* Emoji live in the colour atlas with their own packing and a negative-UV
     * signalling convention, so they cannot use the alpha rasteriser below —
     * but they DO need a raster at the drawn size: magnifying the terminal-cell
     * copy makes a Markdown H1's emoji look soft beside its headline text. Give
     * them their own colour raster under the sized key.
     *
     * Linux needs this branch for a second reason: without it an emoji would
     * fall through to the alpha rasteriser below, which shapes out of the TEXT
     * face. That face has no emoji glyph, so the sized key would cache either a
     * .notdef box or a miss — a wrong emoji, not merely a soft one.
     * rasterize_color_boxed and color_glyph_fill_rgba are both cross-platform
     * (font_ensure_color_atlas_gl backs the GL side). */
    if (utf8_is_emoji(cp)) {
        u32 ekey = sized_cache_key(cp, bw);
        if (!ekey) return font_get_glyph_rect(atlas, cp, out);
        CachedGlyph *ec = cache_lookup(p, ekey);
        if (!ec) {
            i32 ebh = (i32)roundf((f32)bw * (atlas->cell_height / atlas->cell_width));
            /* Bind the face and hold it, exactly as rasterize_glyph and
             * font_check_ligature do. This path reaches face state:
             * rasterize_color_boxed -> color_glyph_fill_rgba reads
             * g_font_ascent, and on Linux g_emoji_font_path, both of which
             * face_load_locked rewrites on a worker. ct_rasterize_color_glyph
             * discards the ascent it is handed (`(void)ascent_px;`), but
             * anything in the colour path that uses the face would otherwise be
             * an unprotected read against a worker that frees exactly those
             * objects.
             *
             * Only the raster is wrapped: the lock is not recursive, and both
             * the cache lookup above and the font_get_glyph_rect fallback below
             * take it themselves. */
            pthread_mutex_lock(&g_face_mu);
            if (face_bind_locked(atlas))
                ec = rasterize_color_boxed(atlas, cp, ekey,
                                           bw * (utf8_char_width(cp) >= 2 ? 2 : 1), ebh);
            pthread_mutex_unlock(&g_face_mu);
        }
        if (ec && !ec->exhausted) {
            if (rect_serve_if_unready(atlas, p, ec->content_version, out)) return true;
            rect_from_cached(ec, out);
            return true;
        }
        return font_get_glyph_rect(atlas, cp, out);
    }
#endif
    /* A non-emoji cluster needs the shaper, which the sized rasteriser does
     * not run; draw it from the shared cell-sized copy. */
    if (utf8_is_cluster(cp))
        return font_get_glyph_rect(atlas, cp, out);
    /* Same aspect the renderer draws the quad at, so the raster maps 1:1 onto
     * the pixels on screen. */
    i32 bh = (i32)roundf((f32)bw * (atlas->cell_height / atlas->cell_width));
    if (bw <= 0 || bh <= 0) return font_get_glyph_rect(atlas, cp, out);

    u32 key = sized_cache_key(cp, bw);
    if (!key) return font_get_glyph_rect(atlas, cp, out);
    CachedGlyph *cached = cache_lookup(p, key);
    if (cached) {
        if (cached->exhausted) { rect_from_cached(cached, out); return false; }
        if (rect_serve_if_unready(atlas, p, cached->content_version, out)) return true;
        rect_from_cached(cached, out);
        return true;
    }

    i32 raster_w = font_glyph_is_wide(cp) ? bw * 2 : bw;
    if (!font_ensure_atlas_room(atlas, raster_w, bh))
        return font_get_glyph_rect(atlas, cp, out);

    u8 *tmp = calloc((usize)raster_w, (usize)bh);
    if (!tmp) return font_get_glyph_rect(atlas, cp, out);
    if (!rasterize_alpha_bitmap_m(atlas, cp, tmp, raster_w, bh, (f32)bw / atlas->cell_width, 0)) {
        free(tmp);
        return font_get_glyph_rect(atlas, cp, out);
    }

    i32 ax = p->pack_x + 1;
    i32 ay = p->pack_y + 1;
    atlas_mirror_r8(atlas, ax, ay, raster_w, bh, tmp);
    if (!font_gpu_upload(atlas, atlas->metal_texture, atlas->texture_id, GL_RED,
                         ax, ay, raster_w, bh, tmp, (usize)raster_w * (usize)bh)) {
        free(tmp);
        p->served_placeholder = true;
        return font_get_glyph_rect(atlas, cp, out);   /* the cell-sized copy */
    }
    free(tmp);

    p->pack_x += raster_w + 2;
    if (bh + 2 > p->pack_row_h) p->pack_row_h = bh + 2;
    p->packed_since_reset++;

    CachedGlyph *ins = cache_insert(p, key, (f32)ax, (f32)ay, (f32)(ax + raster_w),
                                    (f32)(ay + bh), FONT_GLYPH_ALPHA);
    if (rect_serve_if_unready(atlas, p, ins->content_version, out)) return true;
    rect_from_cached(ins, out);
    return true;
}

bool font_get_glyph_rect_for_batch(FontAtlas *atlas, u32 cp, i32 cell_w, FontGlyphRect *out) {
    const FontAtlas *previous=s_batch_atlas;
    s_batch_atlas=atlas;
    bool result=cell_w>0 ? font_get_glyph_rect_sized(atlas,cp,cell_w,out)
                         : font_get_glyph_rect(atlas,cp,out);
    s_batch_atlas=previous;
    return result;
}

void font_warm_text_glyphs_n(FontAtlas *atlas, const char *utf8_text, usize len) {
    FontAtlasPriv *priv = fa_priv(atlas);
    if (!atlas || !priv || !utf8_text) return;
    const u8 *p = (const u8 *)utf8_text;
    usize i = 0;
    while (i < len && (len != (usize)-1 || p[i] != 0)) {
        usize remain = (len == (usize)-1) ? 4 : (len - i);
        if (remain > 4) remain = 4;
        u32 cp = 0;
        u32 n = utf8_decode(p + i, remain, &cp);
        if (n == 0) { i++; continue; }
        i += n;
        /* ASCII printable lives in atlas->glyphs[32..126] from the
         * warm-on-boot pass; nothing to do. Controls and DEL are not
         * rendered anyway. */
        if (cp < 127) continue;
        if (cache_lookup(priv, cp)) continue;
        /* Sync rasterize. Goes through the same path the worker uses for
         * the bitmap, but the atlas mutation happens here (main thread),
         * so the cache entry is installed before this returns. */
        (void)rasterize_glyph(atlas, cp);
    }
}

void font_warm_text_glyphs(FontAtlas *atlas, const char *utf8_text) {
    font_warm_text_glyphs_n(atlas, utf8_text, (usize)-1);
}

/* =========================================================================
 * Ligature cache -- keyed by codepoint sequence
 *
 * Each entry stores the codepoints forming a ligature, the number of chars,
 * the atlas UV coordinates, and whether the entry has been queried and found
 * to NOT be a ligature (negative cache).
 *
 * Up to 4096 slots per atlas: a single 160x44 screen
 * of code contains ~1000+ unique 2-4 char sequences even after the
 * punctuation-opener gate, and the negative cache must hold a full screen's
 * worth or every scroll frame re-shapes through CoreText. The entry type and
 * the size constants are declared at the top of the file. FontAtlasPriv owns
 * a lazily allocated table, initially 256 slots, because its texel rectangles
 * address one specific atlas texture.
 * ========================================================================= */

/* (font_rescale_uv / font_rescale_atlas_caches are gone: texel rectangles do
 * not change when the atlas grows, which is the point of them.) */

/* ---------------------------------------------------------------------------
 * Reclaiming atlas AREA
 *
 * The shelf packer only advances. cache_evict_cold() frees metadata slots but
 * gives back no atlas space, so once the shelf reaches the bottom of a
 * 4096x4096 atlas every new codepoint would get a permanent "exhausted"
 * sentinel whose UVs are the SPACE glyph's — drawn as a blank, not a tofu,
 * for the life of the atlas.
 *
 * The colour atlas next door resets its shelf and drops its cache rather than
 * repacking; the same shape applies here, with one addition — the shelf is
 * rewound to the END of the pre-warm rather than to zero, so ASCII, box
 * drawing and block elements keep their pixels and their atlas->glyphs[] UVs.
 * Everything else re-rasterises on demand, which is bounded by what is
 * actually on screen.
 *
 * Stale UVs are handled by the mechanism that already exists for it: bumping
 * raster.completed_version is what makes ui.c drop every row cache, so the
 * next frame rebuilds from fresh UVs.
 * ------------------------------------------------------------------------- */
static void cache_drop_dynamic(FontAtlasPriv *p);

static bool atlas_reclaim_shelf(FontAtlas *atlas) {
    FontAtlasPriv *p = fa_priv(atlas);
    if (!atlas || !p || !p->pack_reset_valid) return false;
    /* Nothing was packed since the last reclamation: the working set does not
     * fit, and resetting again would just re-rasterise the same glyphs into
     * the same space forever. Let the sentinel stand for this episode.
     *
     * EVERY SITE THAT ADVANCES THE SHELF MUST COUNT HERE. There are five — the
     * async drain, the sprite raster, the cluster raster, the synchronous face
     * raster and the sized raster — plus the ligature raster. A markdown or
     * file-browser session packs mostly through the synchronous ones
     * (font_warm_text_glyphs is synchronous, headings go through the sized
     * raster, macOS filenames are clusters), so a site that did not count
     * would leave the counter at 0 after the FIRST reclaim and every later
     * reclaim would be refused. */
    if (p->atlas_resets > 0 && p->packed_since_reset < 64) return false;

    /* From here on this function overwrites cells that EARLIER FRAMES drew
     * from, and up to METAL_FRAME_BUFFER_COUNT-1 of those frames can still be
     * in flight on the GPU sampling the shared atlas texture. The upload path
     * has no staging buffer and no fence that could protect a cell, so the
     * only thing that makes the write safe is the cells not being live —
     * which is exactly what the reclaim makes untrue. Drain committed GPU
     * work first; a reclaim is rare enough (the atlas has to be at its
     * maximum size AND the working set has to overflow it) that a stall here
     * is negligible. */
    /* Stopping it here is not enough, which is why this defers instead.
     *
     * The drain and the on-demand rasteriser both run INSIDE app_render, after
     * renderer_begin_frame has opened a command buffer. That buffer can already
     * hold encoded draws referencing the very cells about to be overwritten, and
     * waiting for previously COMMITTED work does nothing for it: the writes land,
     * the buffer is committed afterwards, and its draws sample the new content.
     *
     * So when something is watching this atlas on a GPU, the reclaim is only
     * REQUESTED here and performed by font_atlas_service_reclaim() at the top of
     * the next frame, before any command buffer exists — the same rule the
     * wallpaper decode follows (it runs before begin_frame so its GPU upload
     * cannot split an encoder). This glyph takes the exhausted path for one frame,
     * which is a cached sentinel rather than a re-queue, and the next reclaim
     * wipes it.
     *
     * With no hook installed there is no GPU to protect, so the reclaim happens
     * inline.
     *
     * Servicing the deferral does not WAIT: the service point asks whether the GPU
     * is idle and, if it is not, asks the CALLER to stop producing frames until it
     * is. Nothing is recorded here — the state that matters is read at the service
     * point, because anything captured mid-frame excludes the draws this frame has
     * yet to encode and every frame submitted after it. */
    if (p->gpu_idle) {
        /* A NEW REQUEST MERGES INTO THE ONE ALREADY IN FLIGHT — it does not restart
         * it.
         *
         * Every glyph that fails to pack while the atlas is full arrives here, and
         * a frame serviced with WAIT goes on to paint, so a screen of varied text
         * asks again on the very next frame. Re-zeroing the wait state each time
         * would keep the phase-A counter from ever reaching its limit: QUIESCE
         * would never be asked for, and with the GPU a couple of frames behind the
         * reclaim could not proceed at all.
         *
         * The counters describe the WAIT, not the request, so they belong to the
         * first request of a cycle and are cleared by atlas_reclaim_clear_request()
         * when that cycle ends. */
        if (!p->reclaim_wanted) {
            p->reclaim_wanted        = true;
            p->reclaim_turns         = 0;
            p->reclaim_quiescing     = false;
            p->reclaim_quiesce_since = 0.0;
        }
        return false;
    }

    return atlas_reclaim_perform(atlas);
}

/* The reclaim proper. No GPU gate in front of it: every caller has either shown
 * that nothing on a GPU is still reading these cells, or there is no GPU behind
 * this atlas at all. */
static bool atlas_reclaim_perform(FontAtlas *atlas) {
    FontAtlasPriv *p = atlas ? fa_priv(atlas) : NULL;
    if (!atlas || !p || !p->pack_reset_valid) return false;

    /* Clear the reclaimed region so a glyph that lands on a smaller box than
     * its predecessor cannot inherit the old ink around its edges.
     *
     * THE MIRROR IS NOT THE ATLAS. On Metal atlas_bitmap is NULL from the
     * first bulk upload onwards, so clearing it alone would leave the
     * reclaimed pixels on the texture. A new glyph writes its own box in
     * full, so what would survive is the 1 px padding between packed glyphs;
     * the terminal never samples it (nearest, pixel-exact UVs) but UI chrome
     * and the sized rasters do, and would show the previous glyph's edge as
     * a halo. Clear whichever store this atlas actually has. */
    if (atlas->atlas_height > p->pack_reset_y) {
        i32 rows = atlas->atlas_height - p->pack_reset_y;
        usize row_bytes = (usize)atlas->atlas_width * ATLAS_BPP;
        if (atlas->atlas_bitmap) {
            memset(atlas->atlas_bitmap + (usize)p->pack_reset_y * row_bytes,
                   0, (usize)rows * row_bytes);
        }
        /* Up to 16 MiB of zeroes at the atlas maximum, allocated and uploaded
         * once per reclaim — which needs the atlas at its ceiling AND the
         * working set to overflow it, i.e. a handful of times in a session.
         * An allocation failure leaves the stale padding rather than failing
         * the reclaim, which is the direction that keeps text on screen. */
        bool has_gpu =
#ifdef USE_METAL
            (atlas->metal_texture != NULL);
#else
            (atlas->texture_id != 0);
#endif
        if (has_gpu) {
#ifdef USE_METAL
            /* A queued clear: no 16 MiB of zeroes allocated, copied and
             * staged per reclaim — the backend blits strips of its own zero
             * buffer, in order with everything else in the log. */
            p->content_version++;
            if (!renderer_atlas_clear(atlas->gpu_uploader, atlas->metal_texture,
                                      0, p->pack_reset_y, atlas->atlas_width, rows,
                                      p->content_version)) {
                p->content_version--;
                p->upload_refused++;
            } else if (!atlas->gpu_gated) {
                atlas->gpu_ready_version = p->content_version;
            }
#else
            u8 *zeros = calloc(1, (usize)rows * row_bytes);
            if (zeros) {
                font_gpu_upload(atlas, atlas->metal_texture, atlas->texture_id, GL_RED,
                                0, p->pack_reset_y, atlas->atlas_width, rows,
                                zeros, (usize)rows * row_bytes);
                free(zeros);
            }
#endif
        }
    }

    p->pack_x     = p->pack_reset_x;
    p->pack_y     = p->pack_reset_y;
    p->pack_row_h = p->pack_reset_row_h;
    p->packed_since_reset = 0;
    p->atlas_resets++;

    cache_drop_dynamic(p);

    /* Every row cache downstream is now holding rects into reclaimed space,
     * and those rows never drew a placeholder — so the completion signal
     * cannot reach them. The placement generation is what does. */
    p->placement_generation++;
    p->raster.completed_version++;
    return true;
}

static bool font_ensure_atlas_room(FontAtlas *atlas, i32 w, i32 h) {
#ifdef USE_METAL
    if(!font_metal_ensure_private_alpha(atlas))return false;
#endif
    FontAtlasPriv *p = fa_priv(atlas);
    if (!atlas || !p || w <= 0 || h <= 0) return false;
    if (w + 2 > ATLAS_MAX_SIZE || h + 2 > ATLAS_MAX_SIZE) return false;

    if (p->pack_x + w + 2 > atlas->atlas_width) {
        p->pack_x = 0;
        p->pack_y += p->pack_row_h + 2;
        p->pack_row_h = 0;
    }
    /* Return early only when BOTH dims fit. After the row-wrap pack_x is 0,
     * but a glyph/ligature wider than the current atlas side still overflows the
     * row: `pack_x + w + 2 > atlas_width` remains true. Checking height alone
     * here admitted such a glyph, and the packer's memcpy then wrote `w` bytes
     * into atlas_width-stride rows — cross-row corruption, or a heap OOB write
     * for a sufficiently wide cell near the atlas bottom. Require the width too,
     * and grow on width as well as height below. */
    if (w + 2 <= atlas->atlas_width && p->pack_y + h + 2 <= atlas->atlas_height) return true;

    i32 old_w = atlas->atlas_width;
    i32 old_h = atlas->atlas_height;
    i32 new_w = old_w;
    i32 new_h = old_h;
    while ((p->pack_y + h + 2 > new_h || w + 2 > new_w) &&
           (new_w < ATLAS_MAX_SIZE || new_h < ATLAS_MAX_SIZE)) {
        new_w *= 2;
        new_h *= 2;
        if (new_w > ATLAS_MAX_SIZE) new_w = ATLAS_MAX_SIZE;
        if (new_h > ATLAS_MAX_SIZE) new_h = ATLAS_MAX_SIZE;
    }
    if (new_w == old_w && new_h == old_h) return false;
    if (w + 2 > new_w || p->pack_y + h + 2 > new_h) return false;

    /* Everything drawn so far this frame was measured against the atlas the
     * frame is bound to. The renderer keeps drawing this frame from that
     * texture (the replacement is published at the next frame boundary, see
     * renderer_atlas_texture_replaced), and every glyph packed after this
     * point is served as the placeholder until then — so the flush here is a
     * belt for the OpenGL backend, whose texture object changes in place. */
    if (p->gpu_flush) p->gpu_flush(p->gpu_sync_ctx);

    /* The CPU mirror, when there is one, grows in place. On Metal in steady
     * state there is none: the pixels live only in the texture and are copied
     * GPU-side into the replacement — no readback, no full re-upload. */
    u8 *new_bitmap = NULL;
#ifdef USE_METAL
    bool need_bitmap = atlas->atlas_bitmap != NULL;
#else
    /* OpenGL re-creates the texture from a full CPU image, so it needs the
     * buffer whether or not a mirror was kept (a real GL session keeps none
     * and reads the old pixels back below). */
    bool need_bitmap = true;
#endif
    if (need_bitmap) {
        new_bitmap = calloc(1, (usize)new_w * (usize)new_h * ATLAS_BPP);
        if (!new_bitmap) return false;
        if (atlas->atlas_bitmap) {
            for (i32 row = 0; row < old_h; row++) {
                memcpy(new_bitmap + (usize)row * (usize)new_w * ATLAS_BPP,
                       atlas->atlas_bitmap + (usize)row * (usize)old_w * ATLAS_BPP,
                       (usize)old_w * ATLAS_BPP);
            }
        }
    }

#ifdef USE_METAL
    /* Keep the CPU mirror while the metal texture doesn't exist yet: this is
     * still inside font_atlas_create (the bulk upload in
     * font_atlas_create_metal_texture hasn't run). Dropping it here — which is
     * what happens when a LARGER font's pre-raster grows the atlas — would
     * leave the freshly-created texture empty and ALL glyphs would vanish. Once
     * the texture exists (steady state) the mirror is dropped to save RSS. */
    bool keep_cpu_mirror = (atlas->metal_texture == NULL);
    if (atlas->metal_texture) {
        if (!atlas->metal_device) {
            free(new_bitmap);
            return false;
        }
        id<MTLTexture> new_tex = font_metal_new_atlas_texture(atlas->metal_device,
                                                             MTLPixelFormatR8Unorm,
                                                             new_w, new_h);
        if (!new_tex) {
            free(new_bitmap);
            return false;
        }
        id old_tex = (__bridge id)atlas->metal_texture;
        p->content_version++;                /* the copy is a texel write */
        if (atlas->gpu_uploader) {
            /* Published at the frame boundary: cleared, old block copied,
             * bound, old retired once its last reader completes. */
            renderer_atlas_texture_replaced(atlas->gpu_uploader, FONT_GLYPH_ALPHA,
                                            (__bridge void *)old_tex,
                                            (__bridge void *)new_tex, old_w, old_h);
        } else {
            /* No renderer: headless, or the seed before the first frame. Do the
             * same thing synchronously. */
            renderer_metal_texture_clear_sync((__bridge void *)new_tex);
            renderer_metal_texture_copy_sync((__bridge void *)old_tex,
                                             (__bridge void *)new_tex, old_w, old_h);
            atlas->gpu_ready_version = p->content_version;
        }
        [old_tex release];
        [old_tex release];
        atlas->metal_texture = (__bridge void *)new_tex;
    }
#else
    /* Keep a CPU mirror only when no GL texture exists. Without a context,
     * glyph readback uses the mirror; with a texture, raster uploads and
     * readback use the GPU resource. */
    bool keep_cpu_mirror = (atlas->texture_id == 0);
    if (atlas->texture_id) {
        if (!atlas->atlas_bitmap) {
            u8 *old_bitmap = calloc(1, (usize)old_w * (usize)old_h * ATLAS_BPP);
            if (old_bitmap) {
                glBindTexture(GL_TEXTURE_2D, atlas->texture_id);
                glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_UNSIGNED_BYTE, old_bitmap);
                for (i32 row = 0; row < old_h; row++) {
                    memcpy(new_bitmap + (usize)row * (usize)new_w * ATLAS_BPP,
                           old_bitmap + (usize)row * (usize)old_w * ATLAS_BPP,
                           (usize)old_w * ATLAS_BPP);
                }
                free(old_bitmap);
            }
        }
        glBindTexture(GL_TEXTURE_2D, atlas->texture_id);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, new_w, new_h, 0,
                     GL_RED, GL_UNSIGNED_BYTE, new_bitmap);
        p->content_version++;
        atlas->gpu_ready_version = p->content_version;
    }
#endif

    free(atlas->atlas_bitmap);
    /* Under Metal the CPU mirror only exists to seed the first texture; the
     * grow copies GPU-side. Drop it once a texture exists — the full atlas
     * footprint (2.25 MB at 768², 16 MB at 4096²) of steady-state RSS. */
    if (keep_cpu_mirror) {
        atlas->atlas_bitmap = new_bitmap;
    } else {
        free(new_bitmap);
        atlas->atlas_bitmap = NULL;
    }
    atlas->atlas_width = new_w;
    atlas->atlas_height = new_h;
    /* The pixels were copied to the SAME texel positions, so every cached
     * rectangle is still right: only the extent moved, and the renderer's
     * per-frame binding is what absorbs that. Nothing downstream rebuilds. */
    p->extent_version++;
    p->raster.completed_version++;
    font_atlas_reaccount(atlas);
    fprintf(stderr, "font: atlas grew %dx%d -> %dx%d%s\n", old_w, old_h, new_w, new_h,
            keep_cpu_mirror ? "" : " (mirror released)");
    return true;
}

static u32 ligature_hash(const u32 *cps_in, i32 cnt) {
    u32 h = 2166136261u;
    for (i32 i = 0; i < cnt; i++) {
        h ^= cps_in[i];
        h *= 16777619u;
    }
    return h % LIGATURE_CACHE_SIZE;
}

/* A run's decision is keyed with the cell on either side of the candidate
 * (font_check_ligature_run): it says what the font drew THERE, and a font's
 * contextual rules can draw the same sequence differently elsewhere. */
static u32 ligature_hash_ctx(const u32 *cps_in, i32 cnt, u32 l, u32 r) {
    u32 h = 2166136261u ^ 0x9e3779b9u;
    for (i32 i = 0; i < cnt; i++) { h ^= cps_in[i]; h *= 16777619u; }
    h ^= l; h *= 16777619u;
    h ^= r; h *= 16777619u;
    return h % LIGATURE_CACHE_SIZE;
}

static u32 lig_entry_hash(const LigatureEntry *e) {
    return e->ctx ? ligature_hash_ctx(e->codepoints, e->count, e->ctx_l, e->ctx_r)
                  : ligature_hash(e->codepoints, e->count);
}

static bool lig_keys_equal(const u32 *a, const u32 *b, i32 cnt) {
    for (i32 i = 0; i < cnt; i++) {
        if (a[i] != b[i]) return false;
    }
    return true;
}

static bool lig_cache_resize(FontAtlasPriv *p,u32 capacity) {
    LigatureEntry *next=calloc(capacity,sizeof(*next));if(!next)return false;
    for(u32 i=0;i<p->lig_cap;i++)if(p->lig[i].valid) {
        u32 hash=lig_entry_hash(&p->lig[i]);bool placed=false;
        for(u32 probe=0;probe<16;probe++) {
            u32 index=(hash+probe)&(capacity-1);
            if(!next[index].valid){next[index]=p->lig[i];placed=true;break;}
        }
        if(!placed){free(next);return false;}
    }
    mem_budget_adjust(MEM_FONT_CPU,(isize)((usize)capacity*sizeof(*next))-(isize)((usize)p->lig_cap*sizeof(*next)));
    free(p->lig);p->lig=next;p->lig_cap=capacity;return true;
}

static LigatureEntry *lig_cache_lookup(FontAtlasPriv *p, const u32 *cps_in, i32 cnt) {
    if(!p->lig)return NULL;
    u32 idx = ligature_hash(cps_in, cnt);
    for (u32 i = 0; i < 16; i++) {
        u32 slot = (idx + i) & (p->lig_cap - 1);
        if (!p->lig[slot].valid) return NULL;
        if (!p->lig[slot].ctx && p->lig[slot].count == (u8)cnt &&
            lig_keys_equal(p->lig[slot].codepoints, cps_in, cnt))
            return &p->lig[slot];
    }
    return NULL;
}

static LigatureEntry *lig_cache_lookup_ctx(FontAtlasPriv *p, const u32 *cps_in, i32 cnt,
                                           u32 l, u32 r) {
    if(!p->lig)return NULL;
    u32 idx = ligature_hash_ctx(cps_in, cnt, l, r);
    for (u32 i = 0; i < 16; i++) {
        u32 slot = (idx + i) & (p->lig_cap - 1);
        LigatureEntry *e = &p->lig[slot];
        if (!e->valid) return NULL;
        if (e->ctx && e->ctx_l == l && e->ctx_r == r && e->count == (u8)cnt &&
            lig_keys_equal(e->codepoints, cps_in, cnt))
            return e;
    }
    return NULL;
}

static LigatureEntry *lig_cache_insert_at(FontAtlasPriv *p, const LigatureEntry *key,
                                           i32 width, f32 lu0, f32 lv0, f32 lu1, f32 lv1,
                                           bool negative);

static LigatureEntry *lig_cache_insert(FontAtlasPriv *p, const u32 *cps_in, i32 cnt, i32 width,
                                        f32 lu0, f32 lv0, f32 lu1, f32 lv1,
                                        bool negative) {
    LigatureEntry key = { .count = (u8)cnt };
    for (i32 j = 0; j < cnt && j < LIGATURE_MAX_LEN; j++) key.codepoints[j] = cps_in[j];
    return lig_cache_insert_at(p, &key, width, lu0, lv0, lu1, lv1, negative);
}

/* A run's "no ligature here" (font_check_ligature_run). */
static void lig_cache_insert_ctx(FontAtlasPriv *p, const u32 *cps_in, i32 cnt, u32 l, u32 r) {
    LigatureEntry key = { .count = (u8)cnt, .ctx = true, .ctx_l = l, .ctx_r = r };
    for (i32 j = 0; j < cnt && j < LIGATURE_MAX_LEN; j++) key.codepoints[j] = cps_in[j];
    (void)lig_cache_insert_at(p, &key, 0, 0, 0, 0, 0, true);
}

/* The key (codepoints, count and, for a run's decision, its context) decides
 * the slot; everything about it is written over whatever held the slot. */
static void lig_entry_fill(LigatureEntry *e, const LigatureEntry *key, i32 width,
                           f32 lu0, f32 lv0, f32 lu1, f32 lv1, bool negative, u32 version) {
    *e = *key;
    e->width = (u8)width;
    e->valid = true;
    e->negative = negative;
    e->x0 = lu0; e->y0 = lv0;
    e->x1 = lu1; e->y1 = lv1;
    e->content_version = version;
}

static LigatureEntry *lig_cache_insert_at(FontAtlasPriv *p, const LigatureEntry *key,
                                           i32 width, f32 lu0, f32 lv0, f32 lu1, f32 lv1,
                                           bool negative) {
    if(!p->lig && !lig_cache_resize(p,256))return NULL;
    if(p->lig_used>=p->lig_cap*3/4 && p->lig_cap<LIGATURE_CACHE_SIZE)
        (void)lig_cache_resize(p,p->lig_cap*2);
retry: ;
    u32 idx = lig_entry_hash(key);
    for (u32 i = 0; i < 16; i++) {
        u32 slot = (idx + i) & (p->lig_cap - 1);
        if (!p->lig[slot].valid) {
            LigatureEntry *e = &p->lig[slot];
            p->lig_used++;
            lig_entry_fill(e, key, width, lu0, lv0, lu1, lv1, negative, p->content_version);
            return e;
        }
    }
    if(p->lig_cap<LIGATURE_CACHE_SIZE && lig_cache_resize(p,p->lig_cap*2))goto retry;
    /* Cache full -- overwrite first slot */
    LigatureEntry *e = &p->lig[idx & (p->lig_cap - 1)];
    lig_entry_fill(e, key, width, lu0, lv0, lu1, lv1, negative, p->content_version);
    return e;
}

/* =========================================================================
 * Ligature detection and rasterization via CoreText shaping
 * ========================================================================= */

/* How many glyphs one shaped run may contribute. Shaping N characters usually
 * yields N or fewer, but a decomposing substitution can add one — Monaspace
 * Neon's `!=` is two glyphs both attributed to string index 0. Bounded so the
 * collection below can live on the stack. */
#define LIG_MAX_GLYPHS 8

/* Shapes through the face, so it needs the face bound to THIS atlas — see
 * face_bind_locked(). Split like rasterize_glyph for the same reason.
 *
 * Precondition: the caller (font_check_ligature) has already missed the
 * ligature cache, so this always shapes.
 *
 * ---- WHAT COUNTS AS A LIGATURE ----
 *
 * "Did shaping produce FEWER glyphs than characters" is the wrong test for
 * almost every programming face. There are two families and only one of
 * them collapses:
 *
 *   liga — the sequence becomes a single glyph. GeistMono is the ONLY bundled
 *          face that does this, and its `->` glyph is one advance wide, not
 *          two.
 *   calt — the glyph COUNT is preserved and the glyph IDs change: each cell
 *          keeps a glyph and together they draw the ligature. That is what a
 *          fixed-pitch grid wants, so Fira Code, JetBrains Mono, Cascadia,
 *          Iosevka, Maple Mono, Victor Mono and Commit Mono all work this way
 *          — and the count never drops.
 *
 * So the question is whether the glyphs DIFFER from the per-character ones,
 * not how many there are. */
#if defined(LIU_HAVE_HARFBUZZ) && defined(LIU_HAVE_FREETYPE)
/* Same cell-span contract as CoreText. The face mutex is already held by the
 * caller; FreeType's size, transform and glyph slot are additionally serialized. */
static i32 font_hb_ligature_locked(FontAtlas *atlas, const u32 *cps, i32 count, FontGlyphRect *out) {
    FontAtlasPriv *p=fa_priv(atlas);
    if (!p || !g_ft_active || !g_ft_face || count<2 || count>LIGATURE_MAX_LEN) return 0;
    i32 width=(i32)atlas->cell_width*count,height=(i32)atlas->cell_height;
    usize pixels;
    if (width<=0 || height<=0 || !checked_mul_usize((usize)width,(usize)height,&pixels) || pixels>MB(16)) return 0;
    u8 *bitmap=NULL;bool valid=false,changed=false;
    pthread_mutex_lock(&g_ft_mu);
    FT_Face face=g_ft_face;
    f64 ppem=(f64)g_font_scale*face->units_per_EM;
    hb_font_t *font=NULL;hb_buffer_t *buffer=NULL;
    if (!(ppem>.5) || ppem>2048 || FT_Set_Char_Size(face,0,(FT_F26Dot6)lround(ppem*64),72,72)) goto done;
    font=hb_ft_font_create_referenced(face);buffer=hb_buffer_create();
    if (!font || font==hb_font_get_empty() || !buffer || !hb_buffer_allocation_successful(buffer)) goto done;
    hb_ft_font_set_load_flags(font,g_ft_load_flags);
    hb_buffer_add_codepoints(buffer,cps,count,0,count);
    hb_buffer_set_direction(buffer,HB_DIRECTION_LTR);
    hb_buffer_set_script(buffer,HB_SCRIPT_LATIN);
    hb_buffer_set_language(buffer,hb_language_from_string("en",-1));
    g_lig_shapings++;
    hb_shape(font,buffer,NULL,0);
    if (!hb_buffer_allocation_successful(buffer)) goto done;
    u32 n=0;
    const hb_glyph_info_t *info=hb_buffer_get_glyph_infos(buffer,&n);
    const hb_glyph_position_t *positions=hb_buffer_get_glyph_positions(buffer,NULL);
    if (!n || n>LIGATURE_MAX_LEN*4u) goto done;
    changed=n!=(u32)count;
    f64 advance=0;
    for (u32 i=0;i<n;i++) {
        if (!info[i].codepoint) goto done;
        if (i<(u32)count && info[i].codepoint!=FT_Get_Char_Index(face,cps[i])) changed=true;
        advance+=positions[i].x_advance/64.0;
    }
    if (!changed) { valid=true;goto done; }
    if (!(advance>0) || !isfinite(advance)) goto done;
    f64 stretch=width/advance;
    if (stretch<.125 || stretch>8) goto done;
    bitmap=calloc(1,pixels);if(!bitmap)goto done;
    FT_Matrix transform={(FT_Fixed)lround(stretch*65536),0,0,65536};
    FT_Set_Transform(face,&transform,NULL);
    f64 pen_x=0,pen_y=0;
    for (u32 i=0;i<n;i++) {
        if (FT_Load_Glyph(face,info[i].codepoint,g_ft_load_flags) || FT_Render_Glyph(face->glyph,g_ft_render_mode)) goto done;
        const FT_Bitmap *image=&face->glyph->bitmap;
        if (image->pixel_mode!=FT_PIXEL_MODE_GRAY && image->pixel_mode!=FT_PIXEL_MODE_MONO) goto done;
        f64 fx=floor((pen_x+positions[i].x_offset/64.0)*stretch)+face->glyph->bitmap_left;
        f64 fy=floor(g_font_ascent-pen_y-positions[i].y_offset/64.0)-face->glyph->bitmap_top;
        if (!isfinite(fx) || !isfinite(fy) || fx<INT32_MIN || fx>INT32_MAX || fy<INT32_MIN || fy>INT32_MAX) goto done;
        i32 x=(i32)fx,y=(i32)fy;
        for (u32 row=0;row<image->rows;row++) {
            i32 dy=y+(i32)row;if(dy<0 || dy>=height)continue;
            /* FreeType: `buffer` is the top row and `pitch` (either sign) is
             * the offset to the next row down — no flip for a negative pitch. */
            const u8 *src=image->buffer+(isize)row*(isize)image->pitch;
            for (u32 col=0;col<image->width;col++) {
                i32 dx=x+(i32)col;if(dx<0 || dx>=width)continue;
                u32 a=image->pixel_mode==FT_PIXEL_MODE_MONO?((src[col/8]>>(7-col%8))&1u)*255u:src[col];
                u8 *dst=&bitmap[(usize)dy*(usize)width+(usize)dx];
                *dst=(u8)(a+((u32)*dst*(255u-a)+127u)/255u);
            }
        }
        pen_x+=positions[i].x_advance/64.0;pen_y+=positions[i].y_advance/64.0;
    }
    valid=true;
done:
    FT_Set_Transform(face,NULL,NULL);
    if(buffer)hb_buffer_destroy(buffer);if(font)hb_font_destroy(font);
    pthread_mutex_unlock(&g_ft_mu);
    if(!valid){free(bitmap);return 0;}
    if(!changed){lig_cache_insert(p,cps,count,0,0,0,0,0,true);return 0;}
    if(!font_ensure_atlas_room(atlas,width,height)){free(bitmap);return 0;}
    i32 ax=p->pack_x+1,ay=p->pack_y+1;
    atlas_mirror_r8(atlas,ax,ay,width,height,bitmap);
    bool uploaded=font_gpu_upload(atlas,atlas->metal_texture,atlas->texture_id,GL_RED,
                                   ax,ay,width,height,bitmap,pixels);
    free(bitmap);
    if(!uploaded){p->served_placeholder=true;return 0;}
    p->pack_x+=width+2;if(height+2>p->pack_row_h)p->pack_row_h=height+2;p->packed_since_reset++;
    LigatureEntry *entry=lig_cache_insert(p,cps,count,count,ax,ay,ax+width,ay+height,false);
    if(!entry || !glyph_gpu_ready(atlas,entry->content_version)){p->served_placeholder=true;return 0;}
    *out=(FontGlyphRect){.x0=ax,.y0=ay,.x1=ax+width,.y1=ay+height,.kind=FONT_GLYPH_ALPHA};
    return count;
}
#endif

static i32 font_check_ligature_locked(FontAtlas *atlas, const u32 *cps, i32 count,
                                      FontGlyphRect *out) {
    FontAtlasPriv *p = fa_priv(atlas);
    if (!p || count < 2 || count > LIGATURE_MAX_LEN) return 0;

#ifdef PLATFORM_MACOS
    if (!g_ct_font) return 0;

    /* 1. Convert codepoints to a UniChar string. The run has to map 1:1 onto
     *    cells, so anything outside the BMP is declined rather than silently
     *    dropped: skipping one would report `count` cells of width for a
     *    shaping of fewer characters. */
    UniChar uchars[LIGATURE_MAX_LEN];
    for (i32 i = 0; i < count; i++) {
        if (cps[i] > 0xFFFF) {
            lig_cache_insert(p, cps, count, 0, 0, 0, 0, 0, true);
            return 0;
        }
        uchars[i] = (UniChar)cps[i];
    }
    i32 ulen = count;

    /* 2. The glyphs the ordinary per-cell path would draw. If the primary face
     * is missing any of these characters they resolve through the fallback
     * chain per cell, which this path cannot reproduce — decline. */
    CGGlyph plain[LIGATURE_MAX_LEN];
    if (!CTFontGetGlyphsForCharacters(g_ct_font, uchars, plain, ulen)) {
        lig_cache_insert(p, cps, count, 0, 0, 0, 0, 0, true);
        return 0;
    }

    /* 3. Create attributed string with ligature attribute enabled. */
    CFDictionaryRef lig_attrs = ct_lig_attrs_locked();
    if (!lig_attrs) return 0;

    CFStringRef str = CFStringCreateWithCharacters(NULL, uchars, ulen);
    if (!str) return 0;

    /* Both are checked: CFRelease(NULL) is an unconditional abort and
     * CFArrayGetCount(NULL) faults, so an unchecked allocation failure here
     * would take the process down rather than drop one ligature. The colour
     * path next door checks every one of these too. */
    CFAttributedStringRef astr = CFAttributedStringCreate(NULL, str, lig_attrs);
    if (!astr) { CFRelease(str); return 0; }
    g_lig_shapings++;
    CTLineRef line = CTLineCreateWithAttributedString(astr);
    if (!line) { CFRelease(astr); CFRelease(str); return 0; }

    /* 4. Collect the shaped run and ask whether it differs from the plain
     * glyphs. Positions are kept, not just the glyph IDs — see the placement
     * note below. */
    CGGlyph  sh_glyph[LIG_MAX_GLYPHS];
    CGFloat  sh_x[LIG_MAX_GLYPHS];
    i32      sh_n = 0;
    bool     changed = false;
    bool     usable  = true;
    CFArrayRef runs = CTLineGetGlyphRuns(line);
    CFIndex run_count = CFArrayGetCount(runs);
    for (CFIndex ri = 0; ri < run_count && usable; ri++) {
        CTRunRef run = (CTRunRef)CFArrayGetValueAtIndex(runs, ri);
        /* A substituted run's glyph IDs index ANOTHER font, so they can be
         * neither compared against `plain` nor drawn through g_ct_font. */
        CFDictionaryRef rattrs = CTRunGetAttributes(run);
        CTFontRef rfont = rattrs
            ? (CTFontRef)CFDictionaryGetValue(rattrs, kCTFontAttributeName)
            : NULL;
        if (!rfont || !CFEqual(rfont, g_ct_font)) { usable = false; break; }

        CFIndex c = CTRunGetGlyphCount(run);
        if (c <= 0 || sh_n + (i32)c > LIG_MAX_GLYPHS) { usable = false; break; }
        CGGlyph g[LIG_MAX_GLYPHS];
        CGPoint pos[LIG_MAX_GLYPHS];
        CFIndex sidx[LIG_MAX_GLYPHS];
        CTRunGetGlyphs(run, CFRangeMake(0, c), g);
        CTRunGetPositions(run, CFRangeMake(0, c), pos);
        CTRunGetStringIndices(run, CFRangeMake(0, c), sidx);
        for (CFIndex i = 0; i < c; i++) {
            if (sidx[i] < 0 || sidx[i] >= ulen || g[i] != plain[sidx[i]])
                changed = true;
            sh_glyph[sh_n] = g[i];
            sh_x[sh_n]     = pos[i].x;
            sh_n++;
        }
    }
    if (sh_n != ulen) changed = true;   /* liga: the count dropped */

    /* A run WIDER than the cells its characters occupy cannot be drawn here —
     * the box is `count` cells and the overflow would be clipped. No bundled
     * face does this (a decomposition that adds advance rather than sharing
     * it), but the failure would be a silently half-drawn ligature, so decline
     * and let the per-cell path draw the characters instead. */
    if (usable && sh_n > 0) {
        f32 sx0 = (g_face_adv_px > 0.5f && g_cell_width > 0.5f)
                ? (g_cell_width / g_face_adv_px) : 1.0f;
        f32 rw0 = (f32)CTLineGetTypographicBounds(line, NULL, NULL, NULL) * sx0;
        if (rw0 > (f32)((i32)atlas->cell_width * count) + 0.5f) usable = false;
    }

    i32 result = 0;
    bool upload_refused = false;   /* the renderer took nothing: shape again next time */
    bool glyph_created = false;
    i32 lig_w = (i32)atlas->cell_width * count;
    i32 lig_h = (i32)atlas->cell_height;
    /* ASK FOR THE ROOM FIRST, before the calloc of lig_w * lig_h and the
     * whole CoreText raster: font_ensure_atlas_room refuses anything past
     * ATLAS_MAX_SIZE outright, and at a cell wider than ~1023 px the raster
     * would be a multi-megabyte allocation and a full shaping per candidate
     * run, thrown away. Nothing between here and the pack below moves the
     * shelf (the raster touches CoreText and nothing else), so asking first
     * is free. */
    if (usable && changed && sh_n > 0 &&
        font_ensure_atlas_room(atlas, lig_w, lig_h)) {
        /* Ligature detected — rasterize via an alpha-only context to match the
         * single-codepoint glyph path. */
        u8 *tmp = calloc((usize)lig_w, (usize)lig_h);
        if (tmp) {
            CGContextRef ctx = CGBitmapContextCreate(tmp, (size_t)lig_w, (size_t)lig_h,
                                                     8, (size_t)lig_w, NULL,
                                                     (CGBitmapInfo)kCGImageAlphaOnly);
            if (ctx) {
                CGContextClearRect(ctx, CGRectMake(0, 0, lig_w, lig_h));
                CGContextSetAllowsAntialiasing(ctx, true);
                CGContextSetShouldAntialias(ctx, true);
                /* SAME raster settings the single-glyph path gets from
                 * get_glyph_context(): smoothing and the synthetic weight, so
                 * a ligature is drawn at the same weight as the glyphs around
                 * it.
                 *
                 * This path rasterises at 1x. FONT_OVERSAMPLE is 1 today, so
                 * that matches; changing that constant means giving this
                 * context the same CTM scale and downsample pair. */
                CGContextSetAllowsFontSmoothing(ctx, g_opt_smoothing != FONT_SMOOTH_OFF);
                CGContextSetShouldSmoothFonts(ctx, g_opt_smoothing != FONT_SMOOTH_OFF);
                CGContextSetAllowsFontSubpixelPositioning(ctx, false);
                CGContextSetShouldSubpixelPositionFonts(ctx, false);
                CGContextSetAllowsFontSubpixelQuantization(ctx, false);
                CGContextSetShouldSubpixelQuantizeFonts(ctx, false);
                CGContextSetGrayFillColor(ctx, glyph_fill_grey(), 1.0);
                bool stroke_apart = glyph_stroke_drawn_apart(g_font_weight);
                if (g_font_weight > 0.01f) {
                    CGContextSetGrayStrokeColor(ctx, glyph_fill_grey(), 1.0);
                    CGContextSetLineWidth(ctx, (CGFloat)g_font_weight);
                    CGContextSetLineJoin(ctx, kCGLineJoinRound);
                    CGContextSetTextDrawingMode(ctx, stroke_apart ? kCGTextFill : kCGTextFillStroke);
                } else {
                    CGContextSetTextDrawingMode(ctx, kCGTextFill);
                }

                /* ---- PLACEMENT ----
                 *
                 * The shaper lays the run out on the face's NATURAL advance
                 * while the grid marches on the ROUNDED cell, so letting the
                 * line place itself walks the run off the grid by (advance -
                 * cell) per column. Rescaling the run's own positions by
                 * cell/advance locks every piece to its column while leaving
                 * glyph SIZE alone.
                 *
                 * How much that is depends on where roundf lands, so it varies
                 * per (face, size) rather than being a constant. No face is
                 * safe at all its sizes, which is why this is not gated on
                 * anything.
                 *
                 * The run's POSITIONS are used rather than the string indices,
                 * and that is load-bearing: Monaspace Neon draws `!=` as two
                 * glyphs BOTH attributed to string index 0, separated only by
                 * the positions. Placing by index would stack them in one cell.
                 *
                 * A run narrower than its box is then centred in it, by INK and
                 * not by advance. That is the liga case, and GeistMono is why
                 * the distinction matters: it replaces `->` with a single glyph
                 * one advance wide whose ink hangs a long way to the LEFT of
                 * its origin (the glyph is drawn as if it sat on the second
                 * character). Drawing it at x=0 would clip the arrow's tail off
                 * the bitmap; centring its ADVANCE in the box would leave
                 * `<==>` with an entirely blank fourth cell. Centring the ink
                 * box puts the picture where the eye expects it either way. */
                f32 sx = (g_face_adv_px > 0.5f && g_cell_width > 0.5f)
                       ? (g_cell_width / g_face_adv_px) : 1.0f;
                f32 run_w = (f32)CTLineGetTypographicBounds(line, NULL, NULL, NULL) * sx;
                f32 x_off = g_glyph_pen_x;
                if ((f32)lig_w - run_w > 0.5f) {
                    CGRect gbb[LIG_MAX_GLYPHS];
                    CTFontGetBoundingRectsForGlyphs(g_ct_font, kCTFontOrientationHorizontal,
                                                    sh_glyph, gbb, (CFIndex)sh_n);
                    f32 lo = 1e9f, hi = -1e9f;
                    for (i32 i = 0; i < sh_n; i++) {
                        if (CGRectIsNull(gbb[i]) || CGRectIsEmpty(gbb[i])) continue;
                        f32 gx = (f32)sh_x[i] * sx;
                        f32 l = gx + (f32)CGRectGetMinX(gbb[i]);
                        f32 r = gx + (f32)CGRectGetMaxX(gbb[i]);
                        if (l < lo) lo = l;
                        if (r > hi) hi = r;
                    }
                    /* Ink centring supersedes the face-wide pen nudge, which
                     * exists for exactly the same overhang this measures. */
                    if (hi > lo) x_off = ((f32)lig_w - (hi - lo)) * 0.5f - lo;
                }

                CGPoint draw_at[LIG_MAX_GLYPHS];
                for (i32 i = 0; i < sh_n; i++) {
                    draw_at[i].x = (CGFloat)x_off + sh_x[i] * (CGFloat)sx;
                    draw_at[i].y = (CGFloat)(lig_h - g_font_ascent);
                }
                /* CTFontDrawGlyphs, not CTLineDraw: the line's own layout is
                 * exactly what the rescale above replaces. It also keeps this
                 * path off the text-matrix trap CTLineDraw sets (see
                 * ct_rasterize_cluster_alpha) — nothing here can poison a
                 * shared context, and the matrix is identity by construction
                 * in a context this function just created. */
                CGContextSetTextMatrix(ctx, CGAffineTransformIdentity);
                CTFontDrawGlyphs(g_ct_font, sh_glyph, draw_at, (size_t)sh_n, ctx);
                if (stroke_apart) {
                    CGContextSetTextDrawingMode(ctx, kCGTextStroke);
                    CTFontDrawGlyphs(g_ct_font, sh_glyph, draw_at, (size_t)sh_n, ctx);
                }
                CGContextRelease(ctx);

                {
                    i32 ax = p->pack_x + 1;
                    i32 ay = p->pack_y + 1;
                    atlas_mirror_r8(atlas, ax, ay, lig_w, lig_h, tmp);
                    if (!font_gpu_upload(atlas, atlas->metal_texture, atlas->texture_id, GL_RED,
                                         ax, ay, lig_w, lig_h, tmp, (usize)lig_w * (usize)lig_h)) {
                        /* Refused: declined for this frame and NOT negative-
                         * cached — the cells draw as single glyphs and the
                         * run is shaped again next time. */
                        p->served_placeholder = true;
                        upload_refused = true;
                        result = 0;
                    } else {
                    out->x0 = (f32)ax;
                    out->y0 = (f32)ay;
                    out->x1 = (f32)(ax + lig_w);
                    out->y1 = (f32)(ay + lig_h);
                    out->kind = FONT_GLYPH_ALPHA;

                    p->pack_x += lig_w + 2;
                    if (lig_h + 2 > p->pack_row_h) p->pack_row_h = lig_h + 2;
                    p->packed_since_reset++;
                    result = count;

                    /* Insert into ligature cache */
                    LigatureEntry *le = lig_cache_insert(p, cps, count, result,
                                                         out->x0, out->y0, out->x1, out->y1, false);
                    glyph_created = le != NULL;
                    /* Packed this frame: on a boundary-prepared backend it is
                     * not on the GPU yet. Decline the ligature for THIS lookup
                     * (the cells draw as plain glyphs, which the row will
                     * re-shape once the row is repaired) rather than bind a
                     * rect the frame cannot sample. */
                    if (le && !glyph_gpu_ready(atlas, le->content_version)) {
                        p->served_placeholder = true;
                        result = 0;
                    }
                    }
                }
            }
            free(tmp);
        }
    }

    if (result == 0 && !upload_refused && !glyph_created) {
        /* Not a ligature -- negative cache to avoid re-shaping */
        lig_cache_insert(p, cps, count, 0, 0, 0, 0, 0, true);
    }

    CFRelease(line);
    CFRelease(astr);
    CFRelease(str);
    return result;
#elif defined(LIU_HAVE_HARFBUZZ) && defined(LIU_HAVE_FREETYPE)
    return font_hb_ligature_locked(atlas,cps,count,out);
#else
    /* Minimal builds without a system shaper keep the per-cell fallback. */
    lig_cache_insert(p, cps, count, 0, 0, 0, 0, 0, true);
    (void)atlas; (void)out;
    return 0;
#endif
}

/* Public entry: bind the face and hold it for the duration, the way
 * rasterize_glyph does — this runs on the main thread while other windows'
 * raster workers may rebind the face under the same lock.
 *
 * The CACHE HIT answers before that lock, and deliberately: ui.c asks this for
 * every punctuation-opening cell of every rebuilt row, at up to three run
 * lengths each, and the answer is a cached one nearly every time. The table is
 * per-atlas and every writer is on this thread (the shaping below, the atlas
 * grow's UV rescale, the shelf reclaim's drop), so a hit needs no lock at all
 * — taking g_face_mu for it put a process-wide lock, contended by every other
 * window's raster worker, on the row-rebuild path. */
i32 font_check_ligature(FontAtlas *atlas, const u32 *cps, i32 count,
                        FontGlyphRect *out) {
    FontAtlasPriv *p = fa_priv(atlas);
    if (!p || count < 2 || count > LIGATURE_MAX_LEN || !out) return 0;

    LigatureEntry *cached = lig_cache_lookup(p, cps, count);
    if (cached) {
        if (cached->negative || cached->width == 0) return 0;
        /* Packed but not yet prepared for the GPU: decline for this lookup and
         * mark the row, as font_get_glyph_rect does for a single glyph. */
        if (!glyph_gpu_ready(atlas, cached->content_version)) {
            p->served_placeholder = true;
            return 0;
        }
        out->x0 = cached->x0; out->y0 = cached->y0;
        out->x1 = cached->x1; out->y1 = cached->y1;
        out->kind = FONT_GLYPH_ALPHA;
        return (i32)cached->width;
    }

    pthread_mutex_lock(&g_face_mu);
    i32 w = face_bind_locked(atlas)
          ? font_check_ligature_locked(atlas, cps, count, out) : 0;
    pthread_mutex_unlock(&g_face_mu);
    return w;
}

#ifdef PLATFORM_MACOS
/* Shape the whole run ONCE, in its own context, and mark each position whose
 * glyph is not the one the per-cell path would draw. A position the shaping
 * cannot account for (no glyph, two glyphs, a substituted face) is marked too:
 * the mark only ever sends a candidate on to be shaped, never away from it. */
static bool ct_lig_run_shape_locked(FontLigatureRun *run) {
    i32 n = run->len;
    if (!g_ct_font || n < 2 || n > FONT_LIG_RUN_MAX) return false;
    UniChar u[FONT_LIG_RUN_MAX];
    CGGlyph plain[FONT_LIG_RUN_MAX];
    u8 covered[FONT_LIG_RUN_MAX];
    for (i32 i = 0; i < n; i++) {
        if (run->cps[i] > 0xFFFF) return false;
        u[i] = (UniChar)run->cps[i];
    }
    if (!CTFontGetGlyphsForCharacters(g_ct_font, u, plain, n)) return false;
    CFDictionaryRef attrs = ct_lig_attrs_locked();
    if (!attrs) return false;
    CFStringRef str = CFStringCreateWithCharacters(NULL, u, n);
    if (!str) return false;
    CFAttributedStringRef astr = CFAttributedStringCreate(NULL, str, attrs);
    if (!astr) { CFRelease(str); return false; }
    g_lig_shapings++;
    CTLineRef line = CTLineCreateWithAttributedString(astr);
    if (!line) { CFRelease(astr); CFRelease(str); return false; }

    memset(run->changed, 0, (usize)n);
    memset(covered, 0, (usize)n);
    bool ok = true;
    CFArrayRef runs = CTLineGetGlyphRuns(line);
    CFIndex run_count = runs ? CFArrayGetCount(runs) : 0;
    for (CFIndex ri = 0; ri < run_count && ok; ri++) {
        CTRunRef r = (CTRunRef)CFArrayGetValueAtIndex(runs, ri);
        CFDictionaryRef rattrs = CTRunGetAttributes(r);
        CTFontRef rfont = rattrs ? (CTFontRef)CFDictionaryGetValue(rattrs, kCTFontAttributeName) : NULL;
        if (!rfont || !CFEqual(rfont, g_ct_font)) { ok = false; break; }
        CFIndex c = CTRunGetGlyphCount(r);
        for (CFIndex at = 0; at < c && ok; at += 64) {
            CFIndex k = c - at < 64 ? c - at : 64;
            CGGlyph g[64];
            CFIndex sidx[64];
            CTRunGetGlyphs(r, CFRangeMake(at, k), g);
            CTRunGetStringIndices(r, CFRangeMake(at, k), sidx);
            for (CFIndex i = 0; i < k; i++) {
                if (sidx[i] < 0 || sidx[i] >= n) { ok = false; break; }
                if (covered[sidx[i]] || g[i] != plain[sidx[i]]) run->changed[sidx[i]] = 1;
                covered[sidx[i]] = 1;
            }
        }
    }
    for (i32 i = 0; i < n; i++) if (!covered[i]) run->changed[i] = 1;
    CFRelease(line); CFRelease(astr); CFRelease(str);
    return ok;
}
#endif

/* A candidate that misses the cache is the expensive case: one Core Text
 * shaping each. The row builders ask for up to three candidates at every
 * punctuation cell, and in output that is not code — random punctuation, a
 * base64 or minified blob, a table of symbols — nearly every one is a sequence
 * never seen before, so the cache turns over without a hit and a frame would
 * shape thousands of times. So the RUN the candidate sits in is shaped once, on
 * the first miss, and a candidate whose cells that shaping left alone is
 * recorded as no ligature without being shaped on its own. Only a candidate the
 * run drew differently goes on to font_check_ligature, which still decides —
 * and rasterises — on its own.
 *
 * The one place the answers can differ is a font whose contextual rules turn a
 * ligature OFF next to certain characters: shaped alone the candidate is a
 * ligature, in its run it is not, and the run's answer — what the font's own
 * rules draw for that text — is the one used. That answer is cached with the
 * cell on either side (lig_cache_insert_ctx), never as the sequence's own:
 * JetBrains Mono turns `<*` off inside `<**` and draws it everywhere else, so a
 * context-free "no" learnt in the first place would keep it plain in the
 * second. */
i32 font_check_ligature_run(FontAtlas *atlas, FontLigatureRun *run, i32 at, i32 count,
                            FontGlyphRect *out) {
    FontAtlasPriv *p = fa_priv(atlas);
    if (!p || !run || !out || at < 0 || count < 2 || count > LIGATURE_MAX_LEN ||
        at + count > run->len) return 0;
    const u32 *cps = run->cps + at;
    if (!lig_cache_lookup(p, cps, count)) {
#ifdef PLATFORM_MACOS
        /* The cells either side, as the run's decision is keyed (0 = the
         * run's edge): what the font drew between these two is what this
         * decision says, and nothing about the sequence anywhere else. */
        u32 l = at > 0 ? run->cps[at - 1] : 0;
        u32 r = at + count < run->len ? run->cps[at + count] : 0;
        if (lig_cache_lookup_ctx(p, cps, count, l, r)) return 0;
        if (run->state == FONT_LIG_RUN_UNSHAPED) {
            pthread_mutex_lock(&g_face_mu);
            bool shaped = face_bind_locked(atlas) && ct_lig_run_shape_locked(run);
            pthread_mutex_unlock(&g_face_mu);
            run->state = shaped ? FONT_LIG_RUN_SHAPED : FONT_LIG_RUN_PER_CANDIDATE;
        }
        if (run->state == FONT_LIG_RUN_SHAPED) {
            bool touched = false;
            for (i32 i = at; i < at + count; i++) touched |= run->changed[i] != 0;
            if (!touched) {
                lig_cache_insert_ctx(p, cps, count, l, r);
                return 0;
            }
        }
#endif
    }
    return font_check_ligature(atlas, cps, count, out);
}

u64 font_ligature_shapings(void) {
    return g_lig_shapings;
}

/* =========================================================================
 * Font atlas creation
 * ========================================================================= */
i32 font_prepare_ligature(FontAtlas *atlas, const u32 *cps, i32 count) {
    const FontAtlas *previous=s_batch_atlas;
    s_batch_atlas=atlas;
    FontGlyphRect rect;
    i32 result=font_check_ligature(atlas,cps,count,&rect);
    s_batch_atlas=previous;
    return result;
}

/* The same for a candidate taken from a run (font_check_ligature_run). */
i32 font_prepare_ligature_run(FontAtlas *atlas, FontLigatureRun *run, i32 at, i32 count) {
    const FontAtlas *previous=s_batch_atlas;
    s_batch_atlas=atlas;
    FontGlyphRect rect;
    i32 result=font_check_ligature_run(atlas,run,at,count,&rect);
    s_batch_atlas=previous;
    return result;
}

/* ---- Face measurement helpers ----
 *
 * Cap height is read from the 'H' outline rather than OS/2 sCapHeight: the
 * outline is always present and always accurate, while the table is left at 0
 * by a fair number of the bundled faces.
 *
 * Ink extent is the real top/bottom reach of the printable ASCII set. It is
 * consistently smaller than the declared ascent/descent, and it is the extent
 * the cell actually has to contain — sizing the cell off the declared metrics
 * wastes rows, sizing the baseline off them clips '_' and descenders. */

static f32 stb_cap_ratio(stbtt_fontinfo *info) {
    int x0, y0, x1, y1;
    if (!stbtt_GetCodepointBox(info, 'H', &x0, &y0, &x1, &y1)) return 0.0f;
    if (y1 <= y0) return 0.0f;
    /* ScaleForMappingEmToPixels(1.0) == 1 / unitsPerEm */
    return (f32)(y1 - y0) * stbtt_ScaleForMappingEmToPixels(info, 1.0f);
}

/* Accented capitals reach ~2 px higher than any ASCII glyph, and cedillas hang
 * below. Turkish (İ Ğ Ş Ç Ö Ü) and the rest of Latin-1 are everyday terminal
 * text, so they belong in the extent that sizes the line box. */
static const u32 g_ink_probe_extra[] = {
    0x00C0, 0x00C4, 0x00C7, 0x00CF, 0x00D6, 0x00DC, 0x00DD,   /* À Ä Ç Ï Ö Ü Ý */
    0x011E, 0x0130, 0x015E, 0x00E7, 0x015F, 0x0131,           /* Ğ İ Ş ç ş ı */
};
#define INK_PROBE_EXTRA_COUNT (sizeof(g_ink_probe_extra)/sizeof(g_ink_probe_extra[0]))

/* Iteration order for the ink probe: printable ASCII, then the extras above. */
static u32 ink_probe_cp(u32 i) {
    return (i < 94) ? (33 + i) : g_ink_probe_extra[i - 94];
}
#define INK_PROBE_COUNT (94 + INK_PROBE_EXTRA_COUNT)

/* The row's everyday text: ASCII letters and digits. Brackets, bars and the
 * dollar sign reach past them on most faces, and centring on those set the
 * letters low in the row. */
static bool ink_probe_is_text(u32 cp) {
    return (cp >= '0' && cp <= '9') || (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z');
}

/* `out_text_*` is the same reach over ASCII letters and digits alone: the
 * extent the row's everyday text is centred on (font_center_baseline). */
static void stb_ink_extent(stbtt_fontinfo *info, f32 scale,
                           f32 *out_top, f32 *out_bot,
                           f32 *out_left, f32 *out_right,
                           f32 *out_text_top, f32 *out_text_bot) {
    f32 top = 0.0f, bot = 0.0f, left = 0.0f, right = 0.0f;
    f32 text_top = 0.0f, text_bot = 0.0f;
    for (u32 i = 0; i < INK_PROBE_COUNT; i++) {
        u32 cp = ink_probe_cp(i);
        if (!stbtt_FindGlyphIndex(info, (int)cp)) continue;
        int x0, y0, x1, y1;
        stbtt_GetCodepointBitmapBox(info, (int)cp, scale, scale, &x0, &y0, &x1, &y1);
        if (x1 <= x0 || y1 <= y0) continue;
        /* Bitmap-box y grows downward from the baseline: y0 is above it. */
        if ((f32)(-y0) > top) top = (f32)(-y0);
        if ((f32)y1 > bot) bot = (f32)y1;
        if ((f32)x0 < left) left = (f32)x0;
        if ((f32)x1 > right) right = (f32)x1;
        if (ink_probe_is_text(cp)) {
            if ((f32)(-y0) > text_top) text_top = (f32)(-y0);
            if ((f32)y1 > text_bot) text_bot = (f32)y1;
        }
    }
    *out_top = top;
    *out_bot = bot;
    *out_left = left;
    *out_right = right;
    *out_text_top = text_top;
    *out_text_bot = text_bot;
}

static f32 stb_max_ascii_advance(stbtt_fontinfo *info, f32 scale) {
    f32 max_adv = 0.0f;
    for (u32 cp = 32; cp < 127; cp++) {
        int adv, lsb;
        stbtt_GetCodepointHMetrics(info, (int)cp, &adv, &lsb);
        f32 a = adv * scale;
        if (a > max_adv) max_adv = a;
    }
    return max_adv;
}

/* Horizontal pen offset that pulls a face's ink back inside the cell.
 *
 * Only as much right-shift as the left overhang needs, and never more than the
 * slack on the right — centring the ink *range* instead would let one wide
 * outlier ('W' with a -1 px bearing) push every other glyph off centre, and on
 * a face whose ink already fills the advance (Cousine: ink ends 0.3 px short of
 * the cell) it would trade a clipped left edge for a clipped right one.
 * Capped at 2 px: this is a whole-face nudge, not per-glyph bearings. */
static f32 font_center_pen_x(f32 cell_w, f32 ink_left, f32 ink_right) {
    if (!isfinite(ink_left) || !isfinite(ink_right)) return 0.0f;
    if (ink_left >= -0.05f) return 0.0f;             /* nothing hangs off the left */
    f32 want  = ceilf(-ink_left);
    f32 slack = floorf(cell_w - ink_right);
    f32 pen   = MIN(want, slack);
    return CLAMP(pen, 0.0f, 2.0f);
}

/* Place the baseline so the row's everyday text — the reach of ASCII letters
 * and digits, `text_*` — sits centred in the line box, the leading split evenly
 * above and below. The full extent, `ink_*`, also holds the marks above
 * accented capitals and the hooks below letters: it grows *cell_h when it does
 * not fit the requested box, and the baseline moves only as far as keeps all
 * of it inside the cell. Centring on the full extent would set every row low
 * by half the height of an accent. */
static f32 font_center_baseline(f32 *cell_h, f32 text_top, f32 text_bot,
                                f32 ink_top, f32 ink_bot) {
    f32 ink = ceilf(ink_top) + ceilf(ink_bot);
    if (ink > *cell_h) *cell_h = ink;
    if (!(text_top > 0.0f)) text_top = ink_top;
    if (!(text_bot > 0.0f)) text_bot = ink_bot;
    f32 text = ceilf(text_top) + ceilf(text_bot);
    f32 base = roundf((*cell_h - text) * 0.5f + ceilf(text_top));
    f32 lo = ceilf(ink_top), hi = *cell_h - ceilf(ink_bot);
    return CLAMP(base, lo, hi);
}

#ifdef PLATFORM_MACOS
static void ct_ink_extent(CTFontRef f, f32 *out_top, f32 *out_bot,
                          f32 *out_left, f32 *out_right,
                          f32 *out_text_top, f32 *out_text_bot) {
    f32 top = 0.0f, bot = 0.0f, left = 0.0f, right = 0.0f;
    f32 text_top = 0.0f, text_bot = 0.0f;
    UniChar chars[INK_PROBE_COUNT];
    CGGlyph glyphs[INK_PROBE_COUNT]={0};
    CGRect bounds[INK_PROBE_COUNT];
    for(u32 i=0;i<INK_PROBE_COUNT;i++)chars[i]=(UniChar)ink_probe_cp(i);
    CTFontGetGlyphsForCharacters(f,chars,glyphs,INK_PROBE_COUNT);
    CTFontGetBoundingRectsForGlyphs(f,kCTFontOrientationHorizontal,glyphs,bounds,INK_PROBE_COUNT);
    for (u32 i = 0; i < INK_PROBE_COUNT; i++) {
        if(glyphs[i]==0)continue;
        CGRect r=bounds[i];
        if (CGRectIsNull(r) || CGRectIsEmpty(r)) continue;
        if ((f32)CGRectGetMaxY(r) > top) top = (f32)CGRectGetMaxY(r);
        if ((f32)(-CGRectGetMinY(r)) > bot) bot = (f32)(-CGRectGetMinY(r));
        if ((f32)CGRectGetMinX(r) < left) left = (f32)CGRectGetMinX(r);
        if ((f32)CGRectGetMaxX(r) > right) right = (f32)CGRectGetMaxX(r);
        if (ink_probe_is_text(chars[i])) {
            if ((f32)CGRectGetMaxY(r) > text_top) text_top = (f32)CGRectGetMaxY(r);
            if ((f32)(-CGRectGetMinY(r)) > text_bot) text_bot = (f32)(-CGRectGetMinY(r));
        }
    }
    *out_top = top;
    *out_bot = bot;
    *out_left = left;
    *out_right = right;
    *out_text_top = text_top;
    *out_text_bot = text_bot;
}

/* Widest advance across printable ASCII, not just 'M'. In a true monospace
 * face they are identical; the max is what keeps a face with an oversized
 * glyph (or a not-quite-mono fallback) from being clipped by the cell. */
static f32 ct_max_ascii_advance(CTFontRef f) {
    CGGlyph glyphs[95];
    UniChar chars[95];
    for (u32 i = 0; i < 95; i++) chars[i] = (UniChar)(32 + i);
    if (!CTFontGetGlyphsForCharacters(f, chars, glyphs, 95)) {
        /* Some face in the set has no glyph — fall back to probing 'M'. */
        return (f32)ct_font_probe_advance(f);
    }
    CGSize adv[95];
    CTFontGetAdvancesForGlyphs(f, kCTFontOrientationHorizontal, glyphs, adv, 95);
    f32 max_adv = 0.0f;
    for (u32 i = 0; i < 95; i++)
        if ((f32)adv[i].width > max_adv) max_adv = (f32)adv[i].width;
    return max_adv;
}
#endif

/* Allocate the per-atlas cache/packing/worker block. The pthread objects are
 * heap-resident, so PTHREAD_*_INITIALIZER (which is NOT all-zero on macOS)
 * cannot be used — they have to be initialised explicitly. */
static FontAtlasPriv *fa_priv_create(void) {
    FontAtlasPriv *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    if(!cache_resize(p,512)){free(p);return NULL;}
    /* Packing state and raster rings are charged separately from the growing
     * tables. font_atlas_reaccount covers the atlas pixel buffers only. */
    mem_budget_adjust(MEM_FONT_CPU, (isize)sizeof(*p));
    /* Unwound one at a time rather than short-circuited: the && chain left the
     * objects created before the failure initialised and unreachable, since
     * mu_ready stayed false and fa_priv_destroy skips all four on that flag.
     * Needs an allocation failure inside pthread to reach, which is also why
     * it would never have been noticed. */
    i32 built = 0;
    if (pthread_mutex_init(&p->raster.req_mu, NULL) == 0) {
        built++;
        if (pthread_cond_init(&p->raster.req_cv, NULL) == 0) {
            built++;
            if (pthread_mutex_init(&p->raster.done_mu, NULL) == 0) {
                built++;
                if (pthread_cond_init(&p->raster.done_cv, NULL) == 0) {
                    built++;
                    p->raster.mu_ready = true;
                }
            }
        }
    }
    if (!p->raster.mu_ready) {
        if (built > 2) pthread_mutex_destroy(&p->raster.done_mu);
        if (built > 1) pthread_cond_destroy(&p->raster.req_cv);
        if (built > 0) pthread_mutex_destroy(&p->raster.req_mu);
    }
    return p;
}

/* Tear down a priv block. The worker must already be joined (see
 * font_stop_async_raster) — destroying a locked mutex is undefined. */
static void fa_priv_destroy(FontAtlas *atlas) {
    FontAtlasPriv *p = fa_priv(atlas);
    if (!p) return;
    if (p->raster.mu_ready) {
        pthread_mutex_destroy(&p->raster.req_mu);
        pthread_cond_destroy(&p->raster.req_cv);
        pthread_mutex_destroy(&p->raster.done_mu);
        pthread_cond_destroy(&p->raster.done_cv);
    }
    mem_budget_adjust(MEM_FONT_CPU,-(isize)((usize)p->cache_cap*sizeof(*p->cache)+(usize)p->lig_cap*sizeof(*p->lig)));
    free(p->cache);free(p->lig);
    mem_budget_adjust(MEM_FONT_CPU, -(isize)sizeof(*p));
    free(p);
    atlas->priv = NULL;
}

/* The offset of table `tag` in the loaded face when at least `need` bytes of
 * it lie inside the mapping, else 0. */
static u32 face_table_at(const char *tag, u32 need) {
    if (!g_font_data) return 0;
    u32 t = stbtt__find_table(g_font_info.data, (u32)g_font_info.fontstart, tag);
    if (!t || (usize)t + need > g_font_data_size) return 0;
    return t;
}

static i32 face_s16(u32 at) {
    const u8 *p = g_font_data + at;
    return (i32)(i16)(u16)(((u16)p[0] << 8) | p[1]);
}

/* Underline, strikeout and stroke geometry for the loaded face, in pixels,
 * from its own tables: `post` gives the underline's position (the top of the
 * stroke, below the baseline when negative) and thickness, OS/2 the
 * strikeout's. A face that leaves a value out gets a derived one — an
 * underline one thickness below the baseline, a strikeout centred on the
 * lowercase. `baseline` and `cell_h` are the cell's, in pixels from its top.
 * The stroke every procedurally drawn line uses is the underline's thickness,
 * so lines get heavier with the size and the face, not with the display.
 * Thicknesses round UP to whole pixels: a face's 0.7-1.6 px is one pixel on a
 * standard display and two on a Retina one, the same weight on both, where
 * rounding to nearest drew 14 pt lines a hairline half a point wide at 2x. */
static void face_line_metrics_locked(f32 baseline, f32 cell_h, f32 x_height) {
    f32 scale = g_font_scale;
    f32 ul_pos = 0.0f, ul_th = 0.0f, st_pos = 0.0f, st_th = 0.0f;
    u32 post = face_table_at("post", 12);
    if (post) {
        ul_pos = (f32)face_s16(post + 8) * scale;
        ul_th  = (f32)face_s16(post + 10) * scale;
    }
    u32 os2 = face_table_at("OS/2", 30);
    if (os2) {
        st_th  = (f32)face_s16(os2 + 26) * scale;
        st_pos = (f32)face_s16(os2 + 28) * scale;
    }
    if (!(x_height > 0.0f)) x_height = baseline * 0.5f;
    if (!(ul_th > 0.0f) || ul_th > cell_h * 0.25f) ul_th = x_height * 0.15f;
    i32 line = (i32)ceilf(ul_th - 0.01f);
    if (line < 1) line = 1;

    f32 ul_y = (ul_pos < 0.0f) ? roundf(baseline - ul_pos) : baseline + (f32)line;
    if (ul_y < baseline + 1.0f) ul_y = baseline + 1.0f;
    if (ul_y + (f32)line > cell_h) ul_y = cell_h - (f32)line;
    if (ul_y < 0.0f) ul_y = 0.0f;

    i32 st_h = (st_th > 0.0f && st_th < cell_h * 0.25f) ? (i32)ceilf(st_th - 0.01f) : line;
    if (st_h < 1) st_h = 1;
    f32 st_y = (st_pos > 0.0f && st_pos < baseline)
             ? roundf(baseline - st_pos)
             : roundf(baseline - (x_height + (f32)st_h) * 0.5f);
    if (st_y < 0.0f) st_y = 0.0f;
    if (st_y + (f32)st_h > cell_h) st_y = cell_h - (f32)st_h;

    g_face.underline_y = ul_y;
    g_face.underline_h = (f32)line;
    g_face.strike_y = st_y;
    g_face.strike_h = (f32)st_h;
    g_face.line_px = line;
}

/* Load the shared face at one identity, deriving every metric that depends on
 * it, and publish both into g_face. g_face_mu must be held; the caller owns
 * the atlas and the lock, so a failure here returns false having touched
 * nothing but the shared face.
 *
 * Split out of font_atlas_create so a raster can put the face BACK — see
 * face_bind_locked(). */
static bool face_load_locked(const char *font_path, f32 font_size, f32 dpi_scale,
                             f32 want_weight, f32 width_scale, f32 height_scale) {
    f32 pixel_h = font_size * dpi_scale;
    f32 em_px = pixel_h;
    f32 fm_cell_w = 0.0f, fm_cell_h = 0.0f;
    f32 fm_ascent = 0.0f, fm_descent = 0.0f, fm_line_box = 0.0f;
    /* Validate the candidate mapping before releasing the current face. A
     * missing or malformed replacement must not discard a still-usable mmap,
     * especially when the original file has since been removed from disk. */
    usize next_size = 0;
    u8 *next_data = mmap_font_file(font_path, &next_size);
    if (!next_data) {
        fprintf(stderr, "font: cannot open '%s'\n", font_path);
        return false;
    }

    /* Init font.
     *
     * The probe runs FIRST and replaces stbtt_GetFontOffsetForIndex, because
     * stb takes a pointer and no length: it reads numTables straight out of
     * the header and walks the table directory, so a file that is not a font
     * (or a face with a corrupt numTables) walks off the end of the mapping.
     * `font_path` comes from config.json or Settings' font picker (which
     * checks the file EXTENSION and nothing else) and is PERSISTED, so such a
     * crash would repeat on every launch. See sfnt_probe.h. */
    u32 font_offset = 0;
    stbtt_fontinfo next_info;
    if (!font_probe(next_data, next_size, &font_offset) ||
        !stbtt_InitFont(&next_info, next_data, (int)font_offset)) {
        fprintf(stderr, "font: invalid '%s'\n", font_path);
        unmap_font_file(next_data, next_size);
        return false;
    }
    font_prewarm_clear_locked();
    if (g_font_data) {
        FT_PRIMARY_RELEASE();
        unmap_font_file(g_font_data, g_font_data_size);
    }
    g_font_data = next_data;
    g_font_data_size = next_size;
    g_font_info = next_info;
    g_font_weight = want_weight;

#ifdef LIU_HAVE_FREETYPE
    /* Same mapping, second reader. Sizing happens per glyph in
     * ft_rasterize_glyph_m, so nothing here depends on the em size chosen
     * below. */
    ft_primary_load();
#endif

    /* Both of the gamma's inputs are settled now: g_ft_active by the load just
     * above, g_opt_smoothing by whichever caller bound this face. Publish it
     * so the render path never reads them live. */
    font_publish_text_gamma_locked();

    /* ---- Pick the em size ----
     * Normalised: scale the em so this face's cap height lands on the
     * reference ratio, so "14 pt" is the same physical letter height in every
     * font (see the FONT_REF_CAP_RATIO block). Legacy: em == requested size. */
    if (g_opt_normalize) {
        f32 cap_ratio = stb_cap_ratio(&g_font_info);
        if (cap_ratio > 0.05f && cap_ratio < 2.0f) {
            f32 s = CLAMP(FONT_REF_CAP_RATIO / cap_ratio,
                          FONT_NORM_MIN_SCALE, FONT_NORM_MAX_SCALE);
            em_px = pixel_h * s;
        }
    }
    if (!isfinite(em_px) || em_px < 1.0f || em_px > 1280.0f) em_px = pixel_h;

    /* Em-relative, NOT ScaleForPixelHeight: Core Text sizes a face by its em,
     * so mapping the em is what keeps the stb raster (Linux, and the macOS
     * fallback path) at the same size as the Core Text raster. Mapping
     * ascent+descent onto the pixel height would render stb glyphs smaller
     * than their Core Text counterparts. */
    g_font_scale = stbtt_ScaleForMappingEmToPixels(&g_font_info, em_px);

    int ascent_i, descent_i, line_gap_i;
    stbtt_GetFontVMetrics(&g_font_info, &ascent_i, &descent_i, &line_gap_i);
    f32 face_ascent  = ascent_i * g_font_scale;
    f32 face_descent = -descent_i * g_font_scale;   /* positive, below baseline */
    f32 face_gap     = line_gap_i * g_font_scale;

    /* ---- Line box ----
     * Normalised: a fixed multiple of the *requested* pixel size, so every
     * face gives the same row pitch (the faces' own hhea line boxes span
     * 1.00–1.48 em, a 50 % swing that makes switching fonts feel like the
     * window resized). Legacy: the face's own ascent+descent+gap. */
    f32 line_h = g_opt_normalize
        ? roundf(pixel_h * FONT_REF_LINE_RATIO * g_opt_line_height)
        : ceilf((face_ascent + face_descent + face_gap) * g_opt_line_height);
    if (!isfinite(line_h) || line_h < 2.0f) line_h = 2.0f;

    f32 ink_top = 0.0f, ink_bot = 0.0f, ink_left = 0.0f, ink_right = 0.0f;
    f32 text_top = 0.0f, text_bot = 0.0f;
    stb_ink_extent(&g_font_info, g_font_scale, &ink_top, &ink_bot, &ink_left, &ink_right,
                   &text_top, &text_bot);
    if (ink_top <= 0.0f) ink_top = face_ascent;
    if (ink_bot <= 0.0f) ink_bot = face_descent;

    f32 baseline = font_center_baseline(&line_h, text_top, text_bot, ink_top, ink_bot);

    g_font_ascent = baseline;
    fm_ascent = baseline;
    fm_descent = line_h - baseline;
    fm_line_box = line_h;

    f32 max_adv = stb_max_ascii_advance(&g_font_info, g_font_scale);
    /* roundf for even cell spacing — see Core Text branch below. */
    fm_cell_w  = roundf(max_adv);
    fm_cell_h = line_h;
    g_cell_width  = fm_cell_w;
    g_face_adv_px = max_adv;      /* what the rounding above is about to lose */
    g_glyph_pen_x = font_center_pen_x(fm_cell_w, ink_left, ink_right);

    if (!isfinite(g_font_scale) || !isfinite(fm_ascent) ||
        !isfinite(fm_descent) || !isfinite(fm_line_box) ||
        !isfinite(fm_cell_w) || !isfinite(fm_cell_h) ||
        fm_cell_w < 1.0f || fm_cell_h < 1.0f ||
        fm_cell_w > (f32)((ATLAS_MAX_SIZE - 2) / 2) ||
        fm_cell_h > (f32)(ATLAS_MAX_SIZE - 2)) {
        fprintf(stderr, "font: invalid metrics in '%s'\n", font_path);
        FT_PRIMARY_RELEASE();
        unmap_font_file(g_font_data, g_font_data_size);
        g_font_data = NULL;
        g_font_data_size = 0;
        g_font_scale = 0.0f;
        return false;
    }

#ifdef PLATFORM_MACOS
    /* Create Core Text font for pixel-perfect glyph rendering */
    ct_styled_faces_release();
    if (g_ct_font) { ct_font_cache_forget(g_ct_font); CFRelease(g_ct_font); g_ct_font = NULL; }
    for (i32 i = 0; i < g_ct_fallback_count; i++) {
        if (g_ct_fallbacks[i]) {
            ct_font_cache_forget(g_ct_fallbacks[i]);
            CFRelease(g_ct_fallbacks[i]);
        }
        g_ct_fallbacks[i] = NULL;
    }
    g_ct_fallback_count = 0;

    /* The pooled rasterisation context caches the smoothing/oversample state
     * from whichever face built it — drop it so this face's options apply. */
    if (g_glyph_ctx) { CGContextRelease(g_glyph_ctx); g_glyph_ctx = NULL; }
    free(g_glyph_buf);
    g_glyph_buf = NULL;
    g_glyph_ctx_w = 0;
    g_glyph_ctx_h = 0;

    /* Create CTFont from file path, at the normalised em, with the
     * configured features and axes, and the faces the codepoint map names. */
    g_ct_font = ct_font_apply_options(ct_font_from_file(font_path, em_px), true);
    ct_cpmap_load_locked(em_px);

    if (g_ct_font) {
        /* Core Text metrics are authoritative on macOS — recompute the same
         * quantities the stb block above derived, from the real face.
         *
         * roundf instead of ceilf on the advance: ceil systematically inflates
         * the cell by up to 1 px on a font whose natural advance is e.g. 8.3,
         * leaving glyphs floating left-aligned in a too-wide cell. Round keeps
         * the cell within ±0.5 px of the true advance for even spacing. */
        f32 ct_adv = ct_max_ascii_advance(g_ct_font);
        if (ct_adv > 0.0f) {
            fm_cell_w = roundf(ct_adv);
            g_face_adv_px = ct_adv;   /* the un-rounded advance the shaper uses */
        }
        g_cell_width = fm_cell_w;

        f32 ct_top = 0.0f, ct_bot = 0.0f, ct_left = 0.0f, ct_right = 0.0f;
        f32 ct_text_top = 0.0f, ct_text_bot = 0.0f;
        ct_ink_extent(g_ct_font, &ct_top, &ct_bot, &ct_left, &ct_right, &ct_text_top, &ct_text_bot);
        if (ct_top <= 0.0f) ct_top = (f32)CTFontGetAscent(g_ct_font);
        if (ct_bot <= 0.0f) ct_bot = (f32)CTFontGetDescent(g_ct_font);
        g_glyph_pen_x = font_center_pen_x(fm_cell_w, ct_left, ct_right);

        f32 ct_line = g_opt_normalize
            ? roundf(pixel_h * FONT_REF_LINE_RATIO * g_opt_line_height)
            : ceilf(((f32)CTFontGetAscent(g_ct_font) +
                     (f32)CTFontGetDescent(g_ct_font) +
                     (f32)CTFontGetLeading(g_ct_font)) * g_opt_line_height);
        if (!isfinite(ct_line) || ct_line < 2.0f) ct_line = 2.0f;

        f32 ct_baseline = font_center_baseline(&ct_line, ct_text_top, ct_text_bot, ct_top, ct_bot);

        fm_ascent = ct_baseline;
        fm_descent = ct_line - ct_baseline;
        fm_line_box = ct_line;
        fm_cell_h = ct_line;
        g_font_ascent = ct_baseline;
        fprintf(stderr, "font: %s cell=%.0fx%.0f baseline=%.0f em=%.1f (req %.1f) ink=%.1f/%.1f\n",
                g_opt_normalize ? "normalised" : "native",
                fm_cell_w, fm_cell_h, fm_ascent,
                em_px, pixel_h, ct_top, ct_bot);
    }

    if (!isfinite(fm_ascent) || !isfinite(fm_descent) ||
        !isfinite(fm_line_box) || !isfinite(fm_cell_w) ||
        !isfinite(fm_cell_h) || fm_cell_w < 1.0f ||
        fm_cell_h < 1.0f ||
        fm_cell_w > (f32)((ATLAS_MAX_SIZE - 2) / 2) ||
        fm_cell_h > (f32)(ATLAS_MAX_SIZE - 2)) {
        fprintf(stderr, "font: invalid Core Text metrics in '%s'\n", font_path);
        ct_styled_faces_release();
        if (g_ct_font) {
            CFRelease(g_ct_font);
            g_ct_font = NULL;
        }
        FT_PRIMARY_RELEASE();
        unmap_font_file(g_font_data, g_font_data_size);
        g_font_data = NULL;
        g_font_data_size = 0;
        g_font_scale = 0.0f;
        return false;
    }
#endif

    /* Space the cell without scaling the outlines. Both boot glyphs and later
     * worker/ligature rasters must see these same, integral dimensions. Keep
     * the baseline centred when adding or removing vertical padding. */
    f32 spaced_w = roundf(fm_cell_w * width_scale);
    f32 spaced_h = roundf(fm_cell_h * height_scale);
    if (spaced_w < 1.0f || spaced_h < 1.0f ||
        spaced_w > (f32)((ATLAS_MAX_SIZE - 2) / 2) ||
        spaced_h > (f32)(ATLAS_MAX_SIZE - 2)) {
        g_font_scale = 0.0f;
        return false;
    }
    fm_ascent += floorf((spaced_h - fm_cell_h) * 0.5f);
    fm_cell_w = spaced_w;
    fm_cell_h = spaced_h;
    fm_descent = fm_cell_h - fm_ascent;
    fm_line_box = fm_cell_h;
    g_cell_width = fm_cell_w;
    g_font_ascent = fm_ascent;

    /* Cap height, x-height and the icon height, then the decoration lines. */
    {
        f32 cap = 0.0f, xh = 0.0f;
        int bx0, by0, bx1, by1;
        if (stbtt_GetCodepointBox(&g_font_info, 'H', &bx0, &by0, &bx1, &by1) && by1 > by0)
            cap = (f32)(by1 - by0) * g_font_scale;
        if (stbtt_GetCodepointBox(&g_font_info, 'x', &bx0, &by0, &bx1, &by1) && by1 > by0)
            xh = (f32)(by1 - by0) * g_font_scale;
#ifdef PLATFORM_MACOS
        if (g_ct_font) {
            if (CTFontGetCapHeight(g_ct_font) > 1.0) cap = (f32)CTFontGetCapHeight(g_ct_font);
            if (CTFontGetXHeight(g_ct_font) > 1.0) xh = (f32)CTFontGetXHeight(g_ct_font);
        }
#endif
        if (!(cap > 0.0f)) cap = fm_ascent * 0.7f;
        g_cap_px = cap;
        g_icon_h_px = MIN(fm_cell_h, (2.0f * cap + fm_cell_h) / 3.0f);
        g_icon2_h_px = MIN(fm_cell_h, (cap + fm_cell_h) * 0.5f);
        face_line_metrics_locked(fm_ascent, fm_cell_h, xh);
    }

    /* Publish the face identity + the metrics it produced, so the next window
     * asking for the same font can share instead of reloading (see `reuse`). */
    snprintf(g_face.path, sizeof g_face.path, "%s", font_path);
    g_face.size       = font_size;
    g_face.dpi        = dpi_scale;
    g_face.weight     = g_font_weight;
    g_face.line_height = g_opt_line_height;
    g_face.normalize  = g_opt_normalize;
    g_face.smoothing  = g_opt_smoothing;
    g_face.width_scale = width_scale;
    g_face.height_scale = height_scale;
    memcpy(g_face.features, g_opt_features, sizeof g_face.features);
    memcpy(g_face.variations, g_opt_variations, sizeof g_face.variations);
    memcpy(g_face.cpmap, g_opt_cpmap, sizeof g_face.cpmap);
    g_face.cell_w     = fm_cell_w;
    g_face.cell_h     = fm_cell_h;
    g_face.ascent     = fm_ascent;
    g_face.descent    = fm_descent;
    g_face.line_box   = fm_line_box;

    /* ---- Load fallback fonts for missing glyphs ----
     * Process-shared, like the face itself: a second window on the same font
     * must NOT free_fallbacks() the chain the first window's raster worker is
     * walking. Only a real load reaches here; a window that shares the face
     * never re-enters this function at all. */
    free_fallbacks();
    g_pixel_h = em_px; /* cache for dynamic discovery */

#ifdef PLATFORM_MACOS
    /* Default fallback chain:
     * 1. Symbols Nerd Font (PRIORITY — PUA codepoints for filebrowser icons
     *    must resolve to this font before any system fallback picks up a
     *    neighbouring glyph by accident)
     * 2. Fonts from the local Liu fonts directory
     * 3. System monospace / symbol fallbacks */
    /* Eager: SymbolsNerdFontMono only — the filebrowser renders Nerd-Font PUA
     * icons on every first frame, so deferring it would cause a visible
     * rasterize stall. Everything else is queued for lazy load (see
     * load_lazy_fallback_for_cp), which typically never fires for ASCII-only
     * shell sessions → ~2-4 MB CoreText cache saved at idle. */
    {
        char nf[512];
        snprintf(nf, sizeof(nf), "%s/SymbolsNerdFontMono-Regular.ttf", font_user_dir());
        load_fallback(nf, em_px);
        snprintf(nf, sizeof(nf), "%s/JetBrainsMonoNerdFontMono-Regular.ttf", font_user_dir());
        queue_lazy_fallback(nf, em_px);
        snprintf(nf, sizeof(nf), "%s/FiraCodeNerdFontMono-Regular.ttf", font_user_dir());
        queue_lazy_fallback(nf, em_px);
        snprintf(nf, sizeof(nf), "%s/CaskaydiaCoveNerdFontMono-SemiLight.ttf", font_user_dir());
        queue_lazy_fallback(nf, em_px);
        snprintf(nf, sizeof(nf), "%s/HackNerdFontMono-Regular.ttf", font_user_dir());
        queue_lazy_fallback(nf, em_px);
        snprintf(nf, sizeof(nf), "%s/SauceCodeProNerdFontMono-Regular.ttf", font_user_dir());
        queue_lazy_fallback(nf, em_px);
    }
    queue_lazy_fallback("/System/Library/Fonts/Menlo.ttc", em_px);
    queue_lazy_fallback("/System/Library/Fonts/Apple Symbols.ttf", em_px);
    /* Only monospace and symbol faces are queued. Every other script — CJK,
     * Hangul, Thai, Arabic, Hebrew, the Indic scripts — goes to Core Text's
     * own choice for the codepoint in the user's language (ct_chain_font_for),
     * which picks the system face designed for it. A wide proportional face
     * queued here would answer first for all of them. */
#elif defined(PLATFORM_LINUX)
    /* Default fallback chain for Linux:
     * 1. Symbols Nerd Font (priority — PUA icons for filebrowser)
     * 2. Local Liu fonts directory
     * 3. System compatibility fallback */
    {
        char nf[512];
        snprintf(nf, sizeof(nf), "%s/SymbolsNerdFontMono-Regular.ttf", font_user_dir());
        load_fallback(nf, em_px);
        snprintf(nf, sizeof(nf), "%s/JetBrainsMonoNerdFontMono-Regular.ttf", font_user_dir());
        load_fallback(nf, em_px);
        snprintf(nf, sizeof(nf), "%s/FiraCodeNerdFontMono-Regular.ttf", font_user_dir());
        load_fallback(nf, em_px);
        snprintf(nf, sizeof(nf), "%s/CaskaydiaCoveNerdFontMono-SemiLight.ttf", font_user_dir());
        load_fallback(nf, em_px);
        snprintf(nf, sizeof(nf), "%s/HackNerdFontMono-Regular.ttf", font_user_dir());
        load_fallback(nf, em_px);
        snprintf(nf, sizeof(nf), "%s/SauceCodeProNerdFontMono-Regular.ttf", font_user_dir());
        load_fallback(nf, em_px);
    }
    linux_load_system_fallbacks(em_px);
#endif

    /* Load user-configured fallback fonts (set via font_set_user_fallback_config) */
    for (i32 i = 0; i < g_user_fallback_count; i++) {
        if (g_user_fallback_paths[i][0]) {
            if (load_fallback(g_user_fallback_paths[i], em_px))
                fprintf(stderr, "font: loaded user fallback '%s'\n", g_user_fallback_paths[i]);
        }
    }

    fprintf(stderr, "font: %d fallback font(s) loaded (%d user)\n",
            g_fallback_count, g_user_fallback_count);

    return true;
}

/* Put the shared face back to the identity `atlas` was built with, so this
 * atlas's glyphs are rasterised at ITS size and not at whichever window
 * loaded the face last. g_face_mu must be held.
 *
 * Free and silent in the ordinary case: config (font family, size, weight,
 * metric options) is broadcast to every window, so the identity already
 * matches and this is six scalar compares plus a strcmp. It costs a real
 * reload only when two live atlases genuinely differ — in practice two
 * displays with different backing scales — and then only on the first
 * sighting of a codepoint in that atlas, which is not a hot path. */
static bool face_bind_locked(const FontAtlas *atlas) {
    if (!atlas || !atlas->priv) return false;
    const FontAtlasPriv *pp = (const FontAtlasPriv *)atlas->priv;
    if (!pp->face_path[0]) return false;
    if (g_font_data != NULL && g_font_scale > 0.0f &&
        g_face.size == pp->face_size && g_face.dpi == pp->face_dpi &&
        g_face.weight == pp->face_weight &&
        g_face.line_height == pp->face_line_h &&
        g_face.normalize == pp->face_norm &&
        g_face.smoothing == pp->face_smooth &&
        g_face.width_scale == pp->face_width_scale &&
        g_face.height_scale == pp->face_height_scale &&
        strcmp(g_face.path, pp->face_path) == 0 &&
        strcmp(g_face.features, pp->face_features) == 0 &&
        strcmp(g_face.variations, pp->face_variations) == 0 &&
        strcmp(g_face.cpmap, pp->face_cpmap) == 0) {
        g_opt_line_height = pp->face_line_h;
        g_opt_normalize = pp->face_norm;
        g_opt_smoothing = pp->face_smooth;
        g_font_weight = pp->face_weight;
        font_publish_text_gamma_locked();
        return true;
    }

    /* The metric options are part of the identity and are read by the load,
     * so they travel with it rather than being left on the other window's. */
    g_opt_line_height = pp->face_line_h;
    g_opt_normalize   = pp->face_norm;
    g_opt_smoothing   = pp->face_smooth;
    memcpy(g_opt_features, pp->face_features, sizeof g_opt_features);
    memcpy(g_opt_variations, pp->face_variations, sizeof g_opt_variations);
    memcpy(g_opt_cpmap, pp->face_cpmap, sizeof g_opt_cpmap);
    /* A failed bind must not draw through whichever other face survived. */
    return face_load_locked(pp->face_path, pp->face_size, pp->face_dpi,
                           pp->face_weight, pp->face_width_scale,
                           pp->face_height_scale);
}

static bool font_prewarm_enabled(void) {
    const char *value=getenv("LIU_FONT_PREWARM_CACHE");
    return !value || value[0]!='0';
}

/* Called only while g_face_mu pins the face represented by the template. */
static bool font_prewarm_restore_locked(FontAtlas *atlas,bool same_face) {
    FontAtlasPriv *p=fa_priv(atlas);
    if(!font_prewarm_enabled())font_prewarm_clear_locked();
    FontPrewarm *seed=g_prewarm;
    if(!same_face || !seed || !p || p->cache_cap!=512 ||
       seed->width!=atlas->atlas_width || seed->height!=atlas->atlas_height) {
        g_prewarm_misses++;return false;
    }
    memcpy(atlas->glyphs,seed->glyphs,sizeof(atlas->glyphs));
    memcpy(p->cache,seed->cache,sizeof(seed->cache));
    p->cache_used=seed->cache_used;p->content_version=seed->content_version;p->extent_version=seed->extent_version;
    p->pack_x=seed->pack_x;p->pack_y=seed->pack_y;p->pack_row_h=seed->pack_row_h;
    if(atlas->atlas_bitmap)memcpy(atlas->atlas_bitmap,seed->pixels,seed->pixel_bytes);
#ifndef USE_METAL
    if(atlas->texture_id)glTexSubImage2D(GL_TEXTURE_2D,0,0,0,seed->width,seed->used_height,GL_RED,GL_UNSIGNED_BYTE,seed->pixels);
#endif
    atlas->gpu_ready_version=p->content_version;
    g_prewarm_hits++;return true;
}

static void font_prewarm_save_locked(const FontAtlas *atlas) {
    const FontAtlasPriv *p=fa_priv(atlas);
    /* No GPU readback to populate this CPU cache. Metal already has the
     * startup bitmap; real GL contexts retain their existing direct path. */
    if(!font_prewarm_enabled() || g_face_refs<2 || !p || !atlas->atlas_bitmap ||
       p->cache_cap!=512 || p->cache_used!=255 || atlas->atlas_width!=ATLAS_INIT_SIZE ||
       atlas->atlas_height!=ATLAS_INIT_SIZE)return;
    for(u32 i=0;i<p->cache_cap;i++)if(p->cache[i].valid &&
        (p->cache[i].exhausted || p->cache[i].kind!=FONT_GLYPH_ALPHA))return;
    i32 used_height=p->pack_y+p->pack_row_h;usize pixels,bytes;
    if(used_height<=0 || used_height>atlas->atlas_height ||
       !checked_mul_usize((usize)atlas->atlas_width,(usize)used_height,&pixels) ||
       !checked_add_usize(sizeof(FontPrewarm),pixels,&bytes) || bytes>KB(512))return;
    FontPrewarm *seed=malloc(bytes);if(!seed)return;
    seed->bytes=bytes;seed->pixel_bytes=pixels;seed->width=atlas->atlas_width;seed->height=atlas->atlas_height;seed->used_height=used_height;
    seed->pack_x=p->pack_x;seed->pack_y=p->pack_y;seed->pack_row_h=p->pack_row_h;
    seed->cache_used=p->cache_used;seed->content_version=p->content_version;seed->extent_version=p->extent_version;
    memcpy(seed->glyphs,atlas->glyphs,sizeof(seed->glyphs));memcpy(seed->cache,p->cache,sizeof(seed->cache));
    memcpy(seed->pixels,atlas->atlas_bitmap,pixels);
    font_prewarm_clear_locked();g_prewarm=seed;mem_budget_transient_adjust(MEM_FONT_CPU,(isize)bytes);
}

bool font_atlas_create(FontAtlas *atlas, const char *font_path, f32 font_size, f32 dpi_scale, f32 font_weight) {
    return font_atlas_create_spaced(atlas, font_path, font_size, dpi_scale,
                                    font_weight, 1.0f, 1.0f);
}

static bool font_atlas_create_impl(FontAtlas *atlas, const char *font_path,
                              f32 font_size, f32 dpi_scale, f32 font_weight,
                              f32 width_scale, f32 height_scale,bool start_worker) {
    if (!atlas || !font_path || !font_path[0] ||
        !isfinite(font_size) || !isfinite(dpi_scale) ||
        font_size <= 0.0f || dpi_scale <= 0.0f ||
        !isfinite(width_scale) || !isfinite(height_scale) ||
        width_scale < 0.5f || width_scale > 4.0f ||
        height_scale < 0.5f || height_scale > 4.0f) {
        return false;
    }
    f32 pixel_h = font_size * dpi_scale;
    if (!isfinite(pixel_h) || pixel_h > 1024.0f) return false;

    /* Clamped here but PUBLISHED under g_face_mu below: the raster workers read
     * g_font_weight (synthetic stroke) and g_main_thread, so writing them
     * outside the lock is a data race the moment a second window exists. */
    f32 want_weight = isfinite(font_weight) ? CLAMP(font_weight, 0.0f, 2.0f) : 0.0f;

    /* A bare filename (no path separator) is resolved against the bundled font
     * directory (assets/fonts in the source tree, ~/.config/Liu/fonts otherwise),
     * so config can just say "JetBrainsMonoNerdFontMono-Regular.ttf" instead of an absolute
     * path. Anything containing a '/' is used verbatim. */
    char fp_buf[1024];
    if (font_path && font_path[0] && !strchr(font_path, '/')
    ) {
        if (!liu_path_join(fp_buf, sizeof fp_buf, font_user_dir(), font_path))
            return false;
#ifdef PLATFORM_MACOS
        /* Not a bundled file: an installed face's family or PostScript name
         * ("SF Mono", "Menlo"), resolved to its file. */
        struct stat st;
        char named[1024];
        if (stat(fp_buf, &st) != 0 && font_path_for_name(font_path, named, sizeof named))
            snprintf(fp_buf, sizeof fp_buf, "%s", named);
#endif
        font_path = fp_buf;   /* parameter is a local pointer; fp_buf lives for the call */
    }

    /* Everything from here to the return mutates or depends on the shared face,
     * so hold it against the OTHER atlases' raster workers (see g_face_mu).
     * Taken before the per-atlas alloc so `reuse` reads a stable face. */
    pthread_mutex_lock(&g_face_mu);
    g_main_thread = pthread_self(); /* record main thread for thread-safe discovery */

    /* Take the REQUEST as this atlas's options, under the lock. Everything
     * below — the reuse test, face_load_locked's line box, the identity stamp
     * — reads g_opt_*, and a raster worker's face_bind_locked writes it, so
     * this copy is what stops a bind landing between the setter and here from
     * building the atlas out of another window's options. */
    g_opt_line_height = g_req_line_height;
    g_opt_normalize   = g_req_normalize;
    g_opt_smoothing   = font_smoothing_level(g_req_smoothing, dpi_scale);
    memcpy(g_opt_features, g_req_features, sizeof g_opt_features);
    memcpy(g_opt_variations, g_req_variations, sizeof g_opt_variations);
    memcpy(g_opt_cpmap, g_req_cpmap, sizeof g_opt_cpmap);

    /* Does the process-shared face already hold exactly what this atlas wants?
     * If so, take a reference instead of re-loading: reloading would unmap the
     * file and free_fallbacks() the chain the OTHER windows' raster workers are
     * reading right now, and would re-derive metrics their atlases are already
     * packed against. Only a real change (size / family / weight / metric
     * options) falls through to the full load below. */
    bool reuse = (g_face_refs > 0 && g_font_data != NULL && g_font_scale > 0.0f &&
                  g_face.size == font_size && g_face.dpi == dpi_scale &&
                  g_face.weight == want_weight &&
                  g_face.line_height == g_opt_line_height &&
                  g_face.normalize == g_opt_normalize &&
                  g_face.smoothing == g_opt_smoothing &&
                  g_face.width_scale == width_scale &&
                  g_face.height_scale == height_scale &&
                  strcmp(g_face.path, font_path) == 0 &&
                  strcmp(g_face.features, g_opt_features) == 0 &&
                  strcmp(g_face.variations, g_opt_variations) == 0 &&
                  strcmp(g_face.cpmap, g_opt_cpmap) == 0);

    /* The glyph/ligature caches and the packing cursor belong to THIS atlas —
     * cache is keyed by codepoint only, so a weight/size change re-rasterises
     * everything simply by starting from an empty block.
     *
     * Precondition: `atlas` is either freshly zeroed or has been through
     * font_atlas_destroy. Calling create twice without a destroy leaks the
     * atlas texture and strands that atlas's raster worker. */
    memset(atlas, 0, sizeof(*atlas));
    atlas->priv = fa_priv_create();
    if (!atlas->priv) { pthread_mutex_unlock(&g_face_mu); return false; }

    if (!reuse && !face_load_locked(font_path, font_size, dpi_scale, want_weight,
                                    width_scale, height_scale)) {
        fa_priv_destroy(atlas);
        pthread_mutex_unlock(&g_face_mu);
        return false;
    }

    /* One copy-out for both paths: after face_load_locked the metrics live in
     * g_face exactly as they do on the reuse path, so this is the one place
     * an atlas learns its cell size. */
    atlas->cell_width  = g_face.cell_w;
    atlas->cell_height = g_face.cell_h;
    atlas->ascent      = g_face.ascent;
    atlas->descent     = g_face.descent;
    atlas->line_height = g_face.line_box;
    atlas->underline_y = g_face.underline_y;
    atlas->underline_h = g_face.underline_h;
    atlas->strike_y    = g_face.strike_y;
    atlas->strike_h    = g_face.strike_h;
    atlas->line_px     = g_face.line_px > 0 ? g_face.line_px : 1;

    /* Remember the identity this atlas was built against. The face is shared
     * and the NEXT window to ask for a different size or DPI replaces it, so
     * every raster for this atlas puts it back first (face_bind_locked). */
    {
        FontAtlasPriv *pp0 = fa_priv(atlas);
        snprintf(pp0->face_path, sizeof pp0->face_path, "%s", font_path);
        pp0->face_size   = font_size;
        pp0->face_dpi    = dpi_scale;
        pp0->face_weight = want_weight;
        pp0->face_line_h = g_opt_line_height;
        pp0->face_norm   = g_opt_normalize;
        pp0->face_smooth = g_opt_smoothing;
        pp0->face_width_scale = width_scale;
        pp0->face_height_scale = height_scale;
        memcpy(pp0->face_features, g_opt_features, sizeof pp0->face_features);
        memcpy(pp0->face_variations, g_opt_variations, sizeof pp0->face_variations);
        memcpy(pp0->face_cpmap, g_opt_cpmap, sizeof pp0->face_cpmap);
    }

    atlas->atlas_width = ATLAS_INIT_SIZE;
    atlas->atlas_height = ATLAS_INIT_SIZE;
    atlas->accounted_cpu = 0;   /* a recycled FontAtlas must not inherit one */
    atlas->accounted_gpu = 0;
    atlas->alpha_shared=false;atlas->shared_seed=NULL;atlas->metal_seed_texture=NULL;

#ifdef USE_METAL
    /* Metal: allocate CPU bitmap; texture created later via font_atlas_create_metal_texture */
    atlas->texture_id = 0;
    atlas->metal_texture = NULL;
    atlas->atlas_bitmap = calloc(1, (usize)(ATLAS_INIT_SIZE * ATLAS_INIT_SIZE) * ATLAS_BPP);
    if (!atlas->atlas_bitmap) {
        fa_priv_destroy(atlas);
        /* Only this atlas failed — the shared face still backs whatever other
         * windows are alive, so leave it (and its refcount) untouched. */
        if (g_face_refs == 0) {
#ifdef PLATFORM_MACOS
            ct_styled_faces_release();
            if (g_ct_font) {
                ct_font_cache_forget(g_ct_font);
                CFRelease(g_ct_font);
                g_ct_font = NULL;
            }
#endif
            FT_PRIMARY_RELEASE();
            unmap_font_file(g_font_data, g_font_data_size);
            g_font_data = NULL;
            g_font_data_size = 0;
            g_font_scale = 0.0f;
            memset(&g_face, 0, sizeof g_face);
        }
        pthread_mutex_unlock(&g_face_mu);
        return false;
    }

    /* Color emoji atlas (RGBA, 512x512) */
    atlas->color_atlas_w = 512;
    atlas->color_atlas_h = 512;
    atlas->color_atlas_x = 0;
    atlas->color_atlas_y = 0;
    atlas->color_row_h = 0;
    atlas->color_texture = NULL;
    /* NEVER ALLOCATED IN A REAL SESSION. The colour texture IS the store on
     * both backends; this mirror exists only so the headless suites, which
     * have no GPU, can read packed emoji back out, and they allocate it
     * themselves. Every write site is gated on it being non-NULL, so leaving
     * it NULL costs a real session nothing and saves the RGBA footprint. */
    atlas->color_bitmap = NULL;
    atlas->metal_device = NULL;
#else
    /* Create atlas texture */
    glGenTextures(1, &atlas->texture_id);
    glBindTexture(GL_TEXTURE_2D, atlas->texture_id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    /* Seed with zeros rather than NULL. glTexImage2D with NULL leaves the
     * contents UNDEFINED, and the 1px padding between packed glyphs is never
     * written by any upload; the linear filter below reaches into that padding
     * at every glyph edge, where uninitialised driver memory would show up as
     * speckle around the text. */
    {
        u8 *zeros = calloc(1, (usize)ATLAS_INIT_SIZE * (usize)ATLAS_INIT_SIZE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, ATLAS_INIT_SIZE, ATLAS_INIT_SIZE,
                     0, GL_RED, GL_UNSIGNED_BYTE, zeros);
        free(zeros);
    }
    /* The renderer binds a sampler OBJECT for every glyph draw and that is what
     * actually decides the filtering: nearest for terminal text, which samples
     * 1:1, and linear for UI chrome and the sized rasters, which scale on
     * purpose (renderer.c, renderer_flush_glyphs). Sampler objects override the
     * texture's own state, so these parameters only stand in for a draw that
     * binds none — none does today. Left at LINEAR as the safer default of the
     * two: a stray bilinear tap is a slightly soft glyph, a stray nearest tap
     * on scaled chrome drops whole columns of coverage. */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    /* Normally the CPU mirror is NULL: raster paths upload directly to the
     * texture, and the mirror is rebuilt with glGetTexImage when the atlas
     * grows. Without a GL context, glGenTextures returns texture name 0;
     * allocate a CPU mirror so raster paths can still provide atlas pixels. */
    atlas->atlas_bitmap = atlas->texture_id
        ? NULL
        : calloc(1, (usize)(ATLAS_INIT_SIZE * ATLAS_INIT_SIZE) * ATLAS_BPP);

    /* Color emoji atlas geometry (RGBA, 512x512). Only the DIMENSIONS are set
     * here — the texture itself is created on the first emoji
     * (font_ensure_color_atlas_gl), so a session that never shows one costs no
     * VRAM. The rasterizer treats color_atlas_w == 0 as "no color support", so
     * leaving these unset would silently disable emoji entirely. */
    atlas->color_atlas_w = 512;
    atlas->color_atlas_h = 512;
    atlas->color_atlas_x = 0;
    atlas->color_atlas_y = 0;
    atlas->color_row_h = 0;
    atlas->color_texture = NULL;
    atlas->color_texture_gl = 0;
    atlas->color_bitmap = NULL;   /* The CPU mirror is not used on this backend; color glyph pixels are stored in the GPU texture. */
    atlas->metal_device = NULL;
#endif

    /* Only the prewarm is shared. Dynamic glyphs, versions, packing state
     * and workers remain independently owned by each atlas. */
    if(!font_prewarm_restore_locked(atlas,reuse)) {
        for (u32 cp = 32; cp < 127; cp++) rasterize_glyph_locked(atlas, cp);
        for (u32 cp = 0x2500; cp <= 0x257F; cp++) rasterize_glyph_locked(atlas, cp);
        for (u32 cp = 0x2580; cp <= 0x259F; cp++) rasterize_glyph_locked(atlas, cp);
        font_prewarm_save_locked(atlas);
    }
    /* Total: ~255 pre-cached, ~165 slots remaining for on-demand */

    /* Everything above is addressed through atlas->glyphs[] and is drawn by
     * almost every frame, so it is the boundary a reclamation rewinds TO.
     * Captured here, after the pre-warm and before any on-demand glyph can
     * move the shelf. */
    {
        FontAtlasPriv *pp = fa_priv(atlas);
        if (pp) {
            pp->pack_reset_x     = pp->pack_x;
            pp->pack_reset_y     = pp->pack_y;
            pp->pack_reset_row_h = pp->pack_row_h;
            pp->pack_reset_valid = true;
            pp->packed_since_reset = 0;
            pp->atlas_resets = 0;
        }
    }

    atlas->loaded = true;
    /* The GL path never reaches font_atlas_create_metal_texture, so this is
     * where its CPU mirror and alpha texture first go on the ledger. */
    font_atlas_reaccount(atlas);
    /* This atlas now holds a reference on the shared face; font_atlas_destroy
     * drops it and only the last one out unmaps anything. */
    g_face_refs++;
    fprintf(stderr, "font: loaded '%s' %.0fpt (cell: %.0fx%.0f, atlas: %dx%d)%s\n",
            font_path, font_size, atlas->cell_width, atlas->cell_height,
            atlas->atlas_width, atlas->atlas_height,
            reuse ? " [shared face]" : "");

    /* Face is published; other windows' workers may resume. Release before
     * starting this atlas's worker so it does not immediately block on a lock
     * still held here. */
    pthread_mutex_unlock(&g_face_mu);

    /* kick off the background rasterizer for on-demand glyphs. */
    if(start_worker)font_start_async_raster(atlas);
    return true;
}

bool font_atlas_create_spaced(FontAtlas *atlas,const char *font_path,
                              f32 font_size,f32 dpi_scale,f32 font_weight,
                              f32 width_scale,f32 height_scale) {
    return font_atlas_create_impl(atlas,font_path,font_size,dpi_scale,font_weight,width_scale,height_scale,true);
}

bool font_atlas_replace(FontAtlas *atlas, const char *font_path,
                         f32 font_size, f32 dpi_scale, f32 font_weight,
                         f32 width_scale, f32 height_scale, void *device) {
    if (!atlas) return false;
    FontAtlas next = {0};
    if (!font_atlas_create_impl(&next, font_path, font_size, dpi_scale,
                               font_weight, width_scale, height_scale,false)) {
        /* A failed face load may have replaced shared state. Restore the
         * survivor before any subsequent uncached glyph uses it. */
        pthread_mutex_lock(&g_face_mu);
        face_bind_locked(atlas);
        pthread_mutex_unlock(&g_face_mu);
        return false;
    }
    /* The temporary has no worker. Start once, at the final address, after
     * the successful swap; a failed replacement never starts a thread. */
    bool gpu_ready = true;
#ifdef USE_METAL
    void *upload_device = device ? device : atlas->metal_device;
    if (upload_device) {
        font_atlas_create_metal_texture(&next, upload_device);
        gpu_ready = next.metal_texture != NULL;
    } else if (atlas->metal_texture) {
        gpu_ready = false;
    }
#else
    (void)device;
    if (next.texture_id) {
        GLint w = 0, h = 0;
        glBindTexture(GL_TEXTURE_2D, next.texture_id);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
        gpu_ready = w == next.atlas_width && h == next.atlas_height;
    } else if (atlas->texture_id) {
        gpu_ready = false;
    }
#endif
    if (!gpu_ready) {
        font_atlas_destroy(&next);
        pthread_mutex_lock(&g_face_mu);
        (void)face_bind_locked(atlas);
        pthread_mutex_unlock(&g_face_mu);
        return false;
    }
    FontAtlasPriv *old = fa_priv(atlas);
    if (old) {
        if (old->gpu_flush) old->gpu_flush(old->gpu_sync_ctx);
        if (old->gpu_sync) old->gpu_sync(old->gpu_sync_ctx);
        FontAtlasPriv *p = fa_priv(&next);
        p->gpu_flush = old->gpu_flush;
        p->gpu_sync = old->gpu_sync;
        p->gpu_idle = old->gpu_idle;
        p->gpu_sync_ctx = old->gpu_sync_ctx;
        /* A replacement is a new identity: every rect anyone cached named the
         * OLD atlas. Carry the versions forward past the old ones so nothing
         * downstream can mistake the new atlas's counters for stale readings
         * of the old. The renderer is handed the new textures through
         * renderer_metal_set_atlas after this returns. */
        p->placement_generation = old->placement_generation + 1;
        if (p->content_version <= old->content_version) p->content_version = old->content_version + 1;
        if (p->extent_version <= old->extent_version) p->extent_version = old->extent_version + 1;
        next.gpu_uploader = atlas->gpu_uploader;
        next.gpu_gated = atlas->gpu_gated;
        /* Seeded synchronously (font_atlas_create_metal_texture): ready. */
        next.gpu_ready_version = p->content_version;
    }
    font_atlas_destroy(atlas);
    *atlas = next;
    /* The old worker may have rebound the shared face while next was being
     * uploaded. Publish the replacement's options even if its first frame
     * uses only prewarmed glyphs and never asks the new worker for anything. */
    pthread_mutex_lock(&g_face_mu);
    (void)face_bind_locked(atlas);
    pthread_mutex_unlock(&g_face_mu);
    font_start_async_raster(atlas);
    return true;
}

/* Runs in app_init, i.e. once per WINDOW — so opening a second window puts
 * this 8 KB memset next to face_load_locked's read loop over the same array
 * (it walks g_user_fallback_count and hands each path to load_fallback) on the
 * first window's raster worker. Not memory-unsafe on its own (fixed 512-byte
 * rows, count bounded), but a reader could see the pre-reset count with the
 * rows already zeroed, and a user's configured fallback font would then
 * silently fail to load for the life of that face. */
void font_set_user_fallback_config(const char **paths, i32 count) {
    pthread_mutex_lock(&g_face_mu);
    char previous[MAX_USER_FALLBACK_PATHS][512];
    memcpy(previous,g_user_fallback_paths,sizeof(previous));i32 previous_count=g_user_fallback_count;
    g_user_fallback_count = 0;
    memset(g_user_fallback_paths, 0, sizeof(g_user_fallback_paths));
    for (i32 i = 0; i < count && i < MAX_USER_FALLBACK_PATHS; i++) {
        if (paths[i] && paths[i][0]) {
            snprintf(g_user_fallback_paths[g_user_fallback_count], 512, "%s", paths[i]);
            g_user_fallback_count++;
        }
    }
    if(previous_count!=g_user_fallback_count || memcmp(previous,g_user_fallback_paths,sizeof(previous)))font_prewarm_clear_locked();
    pthread_mutex_unlock(&g_face_mu);
}

void font_atlas_load_user_fallbacks(const char **paths, i32 count) {
    /* Appends to the process-shared fallback chain — exclude the workers.
     * g_pixel_h is face state written by face_load_locked, so its guard is
     * INSIDE the lock: read outside it, the value could be one a worker is
     * rewriting. */
    pthread_mutex_lock(&g_face_mu);
    font_prewarm_clear_locked();
    if (g_pixel_h <= 0) { pthread_mutex_unlock(&g_face_mu); return; }
    for (i32 i = 0; i < count; i++) {
        if (!paths[i] || !paths[i][0]) continue;
        if (load_fallback(paths[i], g_pixel_h)) {
            fprintf(stderr, "font: loaded user fallback '%s'\n", paths[i]);
        }
    }
    pthread_mutex_unlock(&g_face_mu);
}

void font_atlas_destroy(FontAtlas *atlas) {
    if (!atlas) return;
    /* Did this atlas ever complete a create? priv is allocated first and freed
     * on every failure path, so it is the marker for "holds a face reference".
     * Guards double-destroy and destroy-of-a-never-created atlas (renderer_
     * destroy after a failed renderer_init). */
    bool held_face_ref = (atlas->priv != NULL);
    /* halt this atlas's worker first; otherwise it would race with texture/
     * atlas state teardown and new requests vs new atlas dimensions. */
    font_stop_async_raster(atlas);
#ifdef USE_METAL
    font_shared_seed_release(atlas);
    if (atlas->metal_texture) {
        /* metal_texture holds a +2 ref: +1 from newTextureWithDescriptor: and
         * +1 from the explicit retain in font_atlas_create_metal_texture().
         * Release both so the texture is actually deallocated. */
        id tex = (__bridge id)atlas->metal_texture;
        [tex release];
        [tex release];
        atlas->metal_texture = NULL;
        atlas->alpha_shared = false;
    }
#else
    if (atlas->texture_id) {
        glDeleteTextures(1, &atlas->texture_id);
        atlas->texture_id = 0;
    }
    if (atlas->color_texture_gl) {
        GLuint ctex = (GLuint)atlas->color_texture_gl;
        glDeleteTextures(1, &ctex);
        atlas->color_texture_gl = 0;
    }
#endif
    if (atlas->atlas_bitmap) {
        free(atlas->atlas_bitmap);
        atlas->atlas_bitmap = NULL;
    }
#ifdef USE_METAL
    if (atlas->color_texture) {
        /* Matches the +1 from newTextureWithDescriptor: and the explicit
         * +1 retain in font_ensure_color_atlas_metal(). */
        id ctex = (__bridge id)atlas->color_texture;
        [ctex release];
        [ctex release];
        atlas->color_texture = NULL;
    }
    atlas->metal_device = NULL;
#endif
    if (atlas->color_bitmap) {
        free(atlas->color_bitmap);
        atlas->color_bitmap = NULL;
    }
    atlas->loaded = false;
    /* Every buffer above is NULL now, so this posts the exact subtraction —
     * whatever this atlas last reported, whichever backend it was on and
     * whichever of the four buffers it actually held. */
    font_atlas_reaccount(atlas);
    /* Glyph cache, ligature cache and packing cursor die with this atlas —
     * they described THIS texture and nothing else. */
    fa_priv_destroy(atlas);

    /* Everything below is PROCESS-SHARED: g_font_info points into g_font_data,
     * so unmapping it while another atlas lives would leave every surviving
     * window rasterising its next box-drawing / powerline / CJK glyph against
     * freed pages. Only the last atlas out gets to do this. */
    if (!held_face_ref) return;
    /* Our own worker is joined (above), but the OTHER atlases' workers are
     * live and read this state — take the face lock, exactly as create does.
     * It has to be taken AFTER the join: a worker blocked on g_face_mu could
     * never finish, and pthread_join would hang. */
    pthread_mutex_lock(&g_face_mu);
    if (g_face_refs > 0) g_face_refs--;
    if (g_face_refs <= 1) font_prewarm_clear_locked();
    if (g_face_refs > 0) { pthread_mutex_unlock(&g_face_mu); return; }

    free_fallbacks();
    if (g_font_data) {
        FT_PRIMARY_RELEASE();
        unmap_font_file(g_font_data, g_font_data_size);
        g_font_data = NULL;
        g_font_data_size = 0;
    }
    g_font_scale = 0.0f;
    g_font_ascent = 0.0f;
    g_glyph_pen_x = 0.0f;
    g_cell_width  = 0.0f;
    g_face_adv_px = 0.0f;
    memset(&g_face, 0, sizeof g_face);
#ifdef PLATFORM_MACOS
    ct_styled_faces_release();
    if (g_ct_font) { ct_font_cache_forget(g_ct_font); CFRelease(g_ct_font); g_ct_font = NULL; }
    if (g_glyph_ctx) { CGContextRelease(g_glyph_ctx); g_glyph_ctx = NULL; }
    free(g_glyph_buf); g_glyph_buf = NULL;
    g_glyph_ctx_w = 0; g_glyph_ctx_h = 0;
#endif
    pthread_mutex_unlock(&g_face_mu);
}

#ifdef USE_METAL
/* Every atlas texture this file creates: PRIVATE storage, written only by GPU
 * blits (renderer_atlas_upload's staging copies, the grow's texture copy) and
 * cleared by a render pass — hence the RenderTarget usage. Private is what a
 * blit destination is meant to be, it is supported on every Metal GPU (Shared
 * textures are not on discrete ones), and it removes any way a CPU write could
 * land in texels a frame in flight is sampling. Returned +2 (the +1 from
 * creation plus the explicit retain the atlas keeps against an autorelease
 * pool draining before the first bind); font_atlas_destroy releases both. */
static id<MTLTexture> font_metal_new_atlas_texture(void *device, MTLPixelFormat fmt,
                                                   i32 w, i32 h) {
    id<MTLDevice> dev = (__bridge id<MTLDevice>)device;
    MTLTextureDescriptor *desc = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:fmt
        width:(NSUInteger)w height:(NSUInteger)h mipmapped:NO];
    desc.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    desc.storageMode = MTLStorageModePrivate;
    id<MTLTexture> tex = [dev newTextureWithDescriptor:desc];
    if (tex) [tex retain];
    return tex;
}

void font_atlas_create_metal_texture(FontAtlas *atlas, void *device) {
    if (!atlas || !device || atlas->atlas_width <= 0 || atlas->atlas_height <= 0)
        return;
    if (atlas->metal_texture) return;
    /* Keep the prewarm page immutable and share it. New glyphs are packed in
     * this window's small private page, so they cannot invalidate another
     * window's texels or require cross-window GPU queue barriers. */
    FontAtlasPriv *p = fa_priv(atlas);
    if (p && font_shared_seed_acquire(atlas,device)) {
        id<MTLTexture> local=(id<MTLTexture>)atlas->metal_seed_texture;
        [local retain];[local retain];
        for (u32 i=0;i<p->cache_cap;i++)
            if (p->cache[i].valid && p->cache[i].kind==FONT_GLYPH_ALPHA)
                p->cache[i].kind=FONT_GLYPH_SEED;
        /* A caller may have shaped a ligature before attaching Metal. Those
         * entries have no page-kind field; rebuild them in the dynamic page. */
        if(p->lig)memset(p->lig,0,p->lig_cap*sizeof(*p->lig));
        p->lig_used=0;
        free(atlas->atlas_bitmap);atlas->atlas_bitmap=NULL;
        atlas->metal_texture=(void *)local;atlas->metal_device=device;atlas->alpha_shared=true;
        atlas->atlas_width=atlas->atlas_height=512;
        p->pack_x=p->pack_y=p->pack_row_h=0;
        p->pack_reset_x=p->pack_reset_y=p->pack_reset_row_h=0;p->pack_reset_valid=true;
        p->content_version++;atlas->gpu_ready_version=p->content_version;
        font_atlas_reaccount(atlas);return;
    }
    /* Glyph atlas is single-channel coverage (R8). The shader broadcasts the
     * red channel as alpha so the fragment can do gamma-correct fg * coverage
     * compositing against a linear-encoded foreground colour. */
    id<MTLTexture> tex = font_metal_new_atlas_texture(device, MTLPixelFormatR8Unorm,
                                                     atlas->atlas_width, atlas->atlas_height);
    if (!tex) return; /* keep the CPU mirror intact for a possible retry */
    atlas->metal_texture = (__bridge void *)tex;

    /* Seed the pre-rasterised bitmap, then free the CPU copy. Immediate on
     * purpose: no renderer is attached yet (renderer_metal_set_atlas follows
     * this call), so this is the one write that cannot race a frame. */
    if (atlas->atlas_bitmap) {
        renderer_metal_upload_texture(atlas->metal_texture, 0, 0,
                                      atlas->atlas_width, atlas->atlas_height,
                                      atlas->atlas_bitmap,
                                      (usize)atlas->atlas_width *
                                      (usize)atlas->atlas_height * ATLAS_BPP);
        free(atlas->atlas_bitmap);
        atlas->atlas_bitmap = NULL;
    } else {
        renderer_metal_texture_clear_sync(atlas->metal_texture);
    }
    if (p) {
        p->content_version++;
        atlas->gpu_ready_version = p->content_version;
    }
    font_atlas_reaccount(atlas);

    /* Remember the device for lazy color-atlas creation; the emoji texture
     * and its CPU bitmap together cost ~1 MB, so creation waits for the
     * first emoji glyph. font_ensure_color_atlas_metal() below performs the
     * actual upload-once-then-retain dance. */
    atlas->metal_device = device;
}

/* Lazy create the emoji RGBA atlas texture + CPU bitmap. Returns true if the
 * atlas is ready to accept emoji uploads (either already created or just now
 * built successfully). Called from rasterize_glyph() on the emoji path. */
bool font_ensure_color_atlas_metal(FontAtlas *atlas) {
    if (!atlas || atlas->color_atlas_w <= 0) return false;
    if (atlas->color_texture) return true;
    if (!atlas->metal_device) return false;

    id<MTLTexture> ctex = font_metal_new_atlas_texture(atlas->metal_device,
                                                      MTLPixelFormatRGBA8Unorm_sRGB,
                                                      atlas->color_atlas_w,
                                                      atlas->color_atlas_h);
    if (!ctex) return false;
    atlas->color_texture = (__bridge void *)ctex;
    /* A fresh private texture holds garbage; the emoji boxes are written in
     * full but the padding between them is sampled by the bilinear colour
     * sampler. Cleared at the frame boundary with the swap, or now when no
     * renderer is attached. */
    if (atlas->gpu_uploader)
        renderer_atlas_texture_replaced(atlas->gpu_uploader, FONT_GLYPH_COLOR, NULL,
                                        (__bridge void *)ctex, 0, 0);
    else
        renderer_metal_texture_clear_sync((__bridge void *)ctex);
    font_atlas_reaccount(atlas);
    fprintf(stderr, "font: color emoji atlas created on demand (%dx%d)\n",
            atlas->color_atlas_w, atlas->color_atlas_h);
    return true;
}
#endif
