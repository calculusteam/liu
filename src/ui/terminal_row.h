/* Pure terminal row geometry and its private asynchronous preparation queue. */
#ifndef LIU_TERMINAL_ROW_H
#define LIU_TERMINAL_ROW_H
#include "ui/ui.h"
#include "terminal/bidi.h"
#include "core/checked.h"
#include "core/utf8.h"
#include "core/mem_budget.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#ifndef LIU_TERM_ROW_REALLOC
#define LIU_TERM_ROW_REALLOC(pointer,bytes) realloc((pointer),(bytes))
#endif
static void term_prepare_cancel(TermRenderCache *c);
static void term_render_cache_spare_free(TermRenderCache *c) {
    free(c->spare_backgrounds);free(c->spare_glyphs);
    c->spare_backgrounds=NULL;c->spare_glyphs=NULL;
    c->spare_bg_cap=0;c->spare_glyph_cap=0;
}
static void term_render_cache_free(TermRenderCache *c) {
    term_prepare_cancel(c);
    free(c->palette_lut);c->palette_lut=NULL;c->palette_source=NULL;c->palette_lut_valid=false;
    renderer_retained_release(&c->gpu_glyphs);
    renderer_retained_release(&c->gpu_backgrounds);
    renderer_retained_release(&c->gpu_decorations);
    term_render_cache_spare_free(c);
    free(c->bg_versions);c->bg_versions=NULL;c->glyph_versions=NULL;
    if (c->bg_rows) {
        for (i32 row = 0; row < c->cached_rows; row++) free(c->bg_rows[row]);
    }
    free(c->bg_rows);       c->bg_rows = NULL;
    free(c->bg_counts);     c->bg_counts = NULL;
    free(c->bg_caps);       c->bg_caps = NULL;
    if (c->glyph_rows) {
        for (i32 row = 0; row < c->cached_rows; row++) free(c->glyph_rows[row]);
    }
    free(c->glyph_rows);    c->glyph_rows = NULL;
    free(c->glyph_counts);  c->glyph_counts = NULL;
    free(c->glyph_caps);    c->glyph_caps = NULL;
    if (c->deco_rows) {
        for (i32 row = 0; row < c->cached_rows; row++) free(c->deco_rows[row]);
    }
    free(c->deco_rows);     c->deco_rows = NULL;
    free(c->deco_counts);   c->deco_counts = NULL;
    free(c->deco_caps);     c->deco_caps = NULL;
    free(c->img_rows);          c->img_rows = NULL;
    free(c->pending_rows);      c->pending_rows = NULL;
    free(c->img_badge_cols);    c->img_badge_cols = NULL;
    c->cached_rows = 0;
    c->cached_cols = 0;
    c->all_rows_dirty = true;
}

    static bool term_render_cache_reserve_rects(TermRenderCache *c, i32 row, i32 min_cap) {
        if (row < 0 || row >= c->cached_rows) return false;
        if (c->bg_caps[row] >= min_cap) return true;

        i32 new_cap = c->bg_caps[row] > 0 ? c->bg_caps[row] : 8;
        while (new_cap < min_cap) new_cap *= 2;

        RectInstance *new_row = LIU_TERM_ROW_REALLOC(c->bg_rows[row], (usize)new_cap * sizeof(RectInstance));
        if (!new_row) { c->row_allocation_failed=true;return false; }
        c->bg_rows[row] = new_row;
        c->bg_caps[row] = new_cap;
        return true;
    }

    static bool term_render_cache_reserve_deco(TermRenderCache *c, i32 row, i32 min_cap) {
        if (row < 0 || row >= c->cached_rows) return false;
        if (c->deco_caps[row] >= min_cap) return true;

        /* Starts at 4, not 8: most rows carry no decoration at all, and the ones
         * that do are usually a link or a diagnostic — a handful of cells, not a
         * full line. A row that really is all underline grows by doubling. */
        i32 new_cap = c->deco_caps[row] > 0 ? c->deco_caps[row] : 4;
        while (new_cap < min_cap) new_cap *= 2;

        RectInstance *new_row = realloc(c->deco_rows[row], (usize)new_cap * sizeof(RectInstance));
        if (!new_row) return false;
        c->deco_rows[row] = new_row;
        c->deco_caps[row] = new_cap;
        return true;
    }

    static bool term_render_cache_reserve_glyphs(TermRenderCache *c, i32 row, i32 min_cap) {
        if (row < 0 || row >= c->cached_rows) return false;
        if (c->glyph_caps[row] >= min_cap) return true;

        i32 new_cap = c->glyph_caps[row] > 0 ? c->glyph_caps[row] : 16;
        while (new_cap < min_cap) new_cap *= 2;

        GlyphInstance *new_row = LIU_TERM_ROW_REALLOC(c->glyph_rows[row], (usize)new_cap * sizeof(GlyphInstance));
        if (!new_row) { c->row_allocation_failed=true;return false; }
        c->glyph_rows[row] = new_row;
        c->glyph_caps[row] = new_cap;
        return true;
    }

    /* Reclaim a per-row buffer that grew to a wide line and now holds far fewer
     * cells, bounding the active pane's steady-state working set. The reserve
     * helpers only grow; this is the matching shrink. Called ONLY on a full
     * rebuild (resize / tab switch / theme-font change — occasional), never on the
     * per-frame dirty-row path, so realloc never thrashes. Shrinks only when a cap
     * is ≥64 and ≥4× oversized vs its current count, to a snug power-of-two. */
    static void term_render_cache_shrink_row(TermRenderCache *c, i32 row) {
        if (row < 0 || row >= c->cached_rows) return;
        i32 bn = c->bg_counts[row];
        if (c->bg_caps[row] >= 64 && c->bg_caps[row] >= bn * 4 + 4) {
            i32 snug = 8; while (snug < bn + 1) snug *= 2;
            if (snug < c->bg_caps[row]) {
                RectInstance *nb = realloc(c->bg_rows[row], (usize)snug * sizeof(RectInstance));
                if (nb) { c->bg_rows[row] = nb; c->bg_caps[row] = snug; }
            }
        }
        i32 gnv = c->glyph_counts[row];
        if (c->glyph_caps[row] >= 64 && c->glyph_caps[row] >= gnv * 4 + 4) {
            i32 snug = 16; while (snug < gnv + 1) snug *= 2;
            if (snug < c->glyph_caps[row]) {
                GlyphInstance *ng = realloc(c->glyph_rows[row], (usize)snug * sizeof(GlyphInstance));
                if (ng) { c->glyph_rows[row] = ng; c->glyph_caps[row] = snug; }
            }
        }
        i32 dn = c->deco_counts[row];
        if (c->deco_caps[row] >= 64 && c->deco_caps[row] >= dn * 4 + 4) {
            i32 snug = 4; while (snug < dn + 1) snug *= 2;
            if (snug < c->deco_caps[row]) {
                RectInstance *nd = realloc(c->deco_rows[row], (usize)snug * sizeof(RectInstance));
                if (nd) { c->deco_rows[row] = nd; c->deco_caps[row] = snug; }
            }
        }
    }

    /* Append one decoration rect to a row's cache, applying the exact transforms
     * renderer_draw_rect applies on the way to the batch — pixel alignment, then
     * sRGB -> linear. Caching the pre-transform values instead would put a
     * different colour on screen. */
    static void deco_push(TermRenderCache *c, i32 row, f32 x, f32 y, f32 w, f32 h,
                          Color col) {
        i32 n = c->deco_counts[row];
        if (!term_render_cache_reserve_deco(c, row, n + 1)) return;
        Color lin = color_linear_from_srgb(col);
        RectInstance *ri = &c->deco_rows[row][n];
        ri->x = floorf(x); ri->y = floorf(y);
        ri->w = ceilf(w);  ri->h = ceilf(h);
        ri->r = lin.r; ri->g = lin.g; ri->b = lin.b; ri->a = lin.a;
        c->deco_counts[row] = n + 1;
    }

    static bool term_render_cache_ensure(TermRenderCache *c,
                                         i32 rows, i32 cols,
                                         f32 cw, f32 ch,
                                         f32 origin_x, f32 origin_y) {
        bool geom_same = (c->cached_rows == rows && c->cached_cols == cols);
        if (!geom_same) {
            term_render_cache_free(c);
            c->bg_rows      = calloc((usize)rows, sizeof(RectInstance *));
            c->bg_counts    = calloc((usize)rows, sizeof(i32));
            c->bg_caps      = calloc((usize)rows, sizeof(i32));
            c->glyph_rows   = calloc((usize)rows, sizeof(GlyphInstance *));
            c->glyph_counts = calloc((usize)rows, sizeof(i32));
            c->glyph_caps   = calloc((usize)rows, sizeof(i32));
            c->deco_rows    = calloc((usize)rows, sizeof(RectInstance *));
            c->deco_counts  = calloc((usize)rows, sizeof(i32));
            c->deco_caps    = calloc((usize)rows, sizeof(i32));
            c->img_rows       = calloc((usize)rows, sizeof(u8));
            c->img_badge_cols = calloc((usize)rows, sizeof(i32));
            c->pending_rows   = calloc((usize)rows, sizeof(u8));
            if (!c->bg_rows || !c->bg_counts || !c->bg_caps ||
                !c->glyph_rows || !c->glyph_counts || !c->glyph_caps ||
                !c->deco_rows || !c->deco_counts || !c->deco_caps ||
                !c->img_rows || !c->img_badge_cols || !c->pending_rows) {
                /* OOM: release whatever succeeded and present an empty but valid
                 * cache (cached_rows == 0) so the caller skips the per-row loop
                 * instead of dereferencing a NULL array. */
                free(c->bg_rows);    free(c->bg_counts);    free(c->bg_caps);
                free(c->glyph_rows); free(c->glyph_counts); free(c->glyph_caps);
                free(c->deco_rows);  free(c->deco_counts);  free(c->deco_caps);
                free(c->img_rows);   free(c->img_badge_cols);
                free(c->pending_rows);
                c->bg_rows = NULL;    c->bg_counts = NULL;    c->bg_caps = NULL;
                c->glyph_rows = NULL; c->glyph_counts = NULL; c->glyph_caps = NULL;
                c->deco_rows = NULL;  c->deco_counts = NULL;  c->deco_caps = NULL;
                c->img_rows = NULL;   c->img_badge_cols = NULL;
                c->pending_rows = NULL;
                c->cached_rows = 0;
                c->cached_cols = 0;
                return false;
            }
            c->cached_rows  = rows;
            c->cached_cols  = cols;
            c->all_rows_dirty = true;
        }
        if (c->cached_cw != cw || c->cached_ch != ch ||
            c->cached_origin_x != origin_x || c->cached_origin_y != origin_y) {
            c->cached_cw = cw; c->cached_ch = ch;
            c->cached_origin_x = origin_x;
            c->cached_origin_y = origin_y;
            c->all_rows_dirty = true;
        }
        return true;
    }

    /* Rebuild a single row's bg + glyph cache entries from the terminal cells.
     * When ligatures are enabled, scans ahead to detect multi-char ligature
     * sequences and emits a single wide glyph instance spanning N cells. */
    /* Programming ligatures commonly begin with punctuation such as ==, =>, !=,
     * >=, <=, &&, ||, ::, .., ++, --, **, ~~, ?., ##, and //. The
     * initial-character gate also accepts _, ;, %, ^, @, $, and backslash
     * because configured ligatures may begin with those codepoints. Letters and
     * digits do not start candidates, so this gate avoids ligature-cache lookups
     * and Core Text shaping for ordinary text. */
    static inline bool lig_can_start(u32 cp) {
        switch (cp) {
        case '=': case '<': case '>': case '!': case '&': case '|':
        case '+': case '-': case ':': case '.': case '~': case '/':
        case '*': case '?': case '_': case '#': case ';': case '%':
        case '^': case '@': case '$': case '\\':
            return true;
        default:
            return false;
        }
    }

    /* The key a cell's glyph is asked for: its codepoint with the style bits of
     * a bold or italic cell (font_style_key), and FONT_ICON_WIDE for an icon
     * whose next cell is a plain blank on the same background, so the icon is
     * drawn across both. `next` is the cell drawn after it (a row's unwritten
     * tail is blanks in the cursor's attributes), NULL in the last column. The
     * row builders, the worker capture and the glyph preparation pass all ask
     * this, so they ask for the same glyph. */
    static inline u32 term_cell_glyph_key(const Cell *c, const Cell *next) {
        u32 key = font_style_key(c->codepoint, (c->attr.flags & ATTR_BOLD) != 0,
                                 (c->attr.flags & ATTR_ITALIC) != 0);
        if (next && font_icon_spills(c->codepoint) &&
            !(c->attr.flags & (ATTR_WIDE | ATTR_WDUMMY))) {
            const Cell *n = next;
            u16 inv = c->attr.flags & ATTR_INVERSE;
            if ((n->codepoint == ' ' || n->codepoint == 0) &&
                !(n->attr.flags & (ATTR_WIDE | ATTR_WDUMMY)) &&
                (n->attr.flags & ATTR_INVERSE) == inv && n->attr.bg == c->attr.bg &&
                (!inv || n->attr.fg == c->attr.fg))
                key |= FONT_ICON_WIDE;
        }
        return key;
    }

    /* A cell a ligature candidate begun at `first` may take in: printable ASCII,
     * not wide, the same colours and the same INVERSE / BOLD / DIM / ITALIC (a
     * ligature is ONE instance in the first cell's colour and style — see the
     * run gather below). */
    static inline bool lig_cell_joins(const Cell *c, const Cell *first) {
        return c->codepoint >= 33 && c->codepoint < 127 &&
               !(c->attr.flags & (ATTR_WDUMMY | ATTR_WIDE)) &&
               c->attr.fg == first->attr.fg && c->attr.bg == first->attr.bg &&
               !((c->attr.flags ^ first->attr.flags) &
                 (ATTR_INVERSE | ATTR_BOLD | ATTR_DIM | ATTR_ITALIC));
    }

    /* Fill `run` (FontLigatureRun) with the cells around `col` that its
     * candidates can be taken from, so font_check_ligature_run shapes them in
     * the context the candidate's cells really have. A run longer than the
     * buffer is a window: up to 64 cells of context to the left, the rest to
     * the right, and `*cut` says the run goes on — the caller fills again
     * before a candidate could reach past it. Returns the window's first
     * column; `*end` is one past its last. */
    static inline i32 lig_run_fill(FontLigatureRun *run, const Cell *cells, i32 len, i32 col,
                                   i32 *end, bool *cut) {
        const Cell *first = &cells[col];
        i32 s = col, e = col + 1;
        while (s > 0 && col - s < 64 && lig_cell_joins(&cells[s - 1], first)) s--;
        while (e < len && e - s < FONT_LIG_RUN_MAX && lig_cell_joins(&cells[e], first)) e++;
        *cut = e < len && lig_cell_joins(&cells[e], first);
        for (i32 i = s; i < e; i++) run->cps[i - s] = cells[i].codepoint;
        run->len = e - s;
        run->state = FONT_LIG_RUN_UNSHAPED;
        *end = e;
        return s;
    }

    /* Resolve an indexed colour against THIS terminal's palette when it has one.
     * Falls back to the process-global LUT, which is the theme's palette and the
     * correct answer for a terminal that never received OSC 4 — the documented
     * "NULL palette means defaults" contract in terminal.h. */
    static inline Color cache_color_resolve(const TermRenderCache *c, u32 val, u32 fallback) {
        if (val & 0x01000000) return color_resolve(val, fallback);   /* truecolor */
        if (c && c->palette_lut_valid) return c->palette_lut[val < 256 ? val : fallback];
        if (c && c->palette_source) {
            u32 entry=c->palette_source[val < 256 ? val : fallback];
            if(entry)return color_resolve(entry,fallback);
        }
        return g_ansi_colors[val < 256 ? val : fallback];
    }

    static void term_render_cache_sync_palette(TermRenderCache *cache,Terminal *term) {
        cache->palette_source=term->palette;
        if(!term->palette) {
            free(cache->palette_lut);cache->palette_lut=NULL;cache->palette_lut_valid=false;
        } else if(term->palette_modified || !cache->palette_lut_valid) {
            if(!cache->palette_lut)cache->palette_lut=malloc(256u*sizeof(Color));
            cache->palette_lut_valid=false;
            if(cache->palette_lut) {
                for(u32 i=0;i<256;i++)cache->palette_lut[i]=cache_color_resolve(cache,i,i);
                cache->palette_lut_valid=true;
            }
            cache->all_rows_dirty=true;
        }
        if(term->palette_modified)cache->all_rows_dirty=true;
        term->palette_modified=false;
    }

    typedef struct {
        bool ligatures, bold_bright, wallpaper;
        f32 opacity, wallpaper_opacity;
        FontAtlas *font; /* main-thread path only; NULL in worker snapshots */
        void *lookup;
        bool (*glyph)(void *,u32,FontGlyphRect *);
        i32 (*ligature)(void *,const u32 *,i32,FontGlyphRect *);
        bool (*emoji)(void *,u32);
        const f32 *decode_lut;
        bool isolated;
        /* A prompt being translated in THIS pane (ui/translate_fx.h): its
         * glyphs take the effect's colour. Zero everywhere else, and in every
         * worker snapshot — a pane with the effect on builds its rows here. */
        TranslateFxPaint fx;
    } TermRowStyle;
    static TermRowStyle term_row_style(AppState *app) {
        return (TermRowStyle){.ligatures=app->config.enable_ligatures,
            .bold_bright=app->config.bold_is_bright,.wallpaper=app->renderer.bg_texture!=0,
            .opacity=app->config.opacity,.wallpaper_opacity=app->renderer.bg_opacity,
            .font=&app->renderer.font};
    }
    /* How long a pane counts as flooding after its last drain that ran out of
     * budget: long enough that a steady stream does not flip ligatures on and
     * off between frames. */
    #define LIG_FLOOD_HOLD_SEC 0.10
    static inline bool term_lig_flooding(const Terminal *t, f64 now) {
        return t && t->lig_flood_until > now;
    }
    /* term_row_style for one pane: no ligatures while it floods (cache->lig_flood,
     * decided by term_pane_draw before anything builds a row). */
    static TermRowStyle term_row_style_pane(AppState *app, const TermRenderCache *c) {
        TermRowStyle s = term_row_style(app);
        if (c && c->lig_flood) s.ligatures = false;
        return s;
    }
    static bool term_style_glyph(const TermRowStyle *s,u32 cp,FontGlyphRect *rect) {
        return s->glyph?s->glyph(s->lookup,cp,rect):font_get_glyph_rect(s->font,cp,rect);
    }
    static i32 term_style_ligature(const TermRowStyle *s,const u32 *cp,i32 n,FontGlyphRect *rect) {
        return s->ligature?s->ligature(s->lookup,cp,n,rect):font_check_ligature(s->font,cp,n,rect);
    }
    static bool term_style_emoji(const TermRowStyle *s,u32 cp) {
        return s->emoji?s->emoji(s->lookup,cp):utf8_is_emoji(cp&~FONT_STYLE_MASK);
    }
    static f32 term_style_decode(const TermRowStyle *s,f32 value) {
        if(!s->decode_lut)return srgb_decode(value);
        if(!(value>0))return 0;
        if(!(value<1))return 1;
        return s->decode_lut[(u32)(value*255.f+.5f)];
    }
    static Color term_style_linear(const TermRowStyle *s,Color value) {
        if(!s->isolated)return color_linear_from_srgb(value);
        return (Color){term_style_decode(s,value.r),term_style_decode(s,value.g),
            term_style_decode(s,value.b),value.a};
    }

#ifdef LIU_TEST_BACKGROUND_RUNS
    /* When `LIU_TEST_BACKGROUND_RUNS` is enabled, preserve the original per-cell
     * geometry for pixel comparisons. */
    static bool term_test_background_runs = true;
#endif
    static inline bool term_row_append_background(TermRenderCache *c, i32 row,
                                                   i32 *count, u32 *last_key, u32 key,
                                                   f32 x, f32 y,
                                                   f32 w, f32 h, Color color) {
        bool merge = *count > 0 && *last_key == key;
#ifdef LIU_TEST_BACKGROUND_RUNS
        merge = merge && term_test_background_runs;
#endif
        if (merge) {
            RectInstance *last = &c->bg_rows[row][*count - 1];
            /* The palette and opacity are fixed for this row, so an identical
             * canonical colour source produces the same linear RGBA. Comparing
             * it first avoids loading four floats for unrelated colours.
             * Snapped fractional cells
             * can overlap; those must stay separate to preserve double blends.
             * Only the last rectangle is considered, retaining painter order. */
            if (last->x + last->w == x && last->y == y && last->h == h) {
                last->w = (x + w) - last->x;
                return true;
            }
        }
        if (!term_render_cache_reserve_rects(c, row, *count + 1)) return false;
        *last_key = key;
        c->bg_rows[row][(*count)++] = (RectInstance){
            .x=x, .y=y, .w=w, .h=h,
            .r=color.r, .g=color.g, .b=color.b, .a=color.a
        };
        return true;
    }

    static void term_render_cache_build_row(TermRenderCache *c, const TermRowStyle *style,
                                            Terminal *term, i32 row,
                                            f32 origin_x, f32 origin_y,
                                            f32 cw, f32 ch) {
        c->row_allocation_failed=false;
        i32 bg_n = 0, gn = 0;
        u32 bg_key = 0;
        GlyphInstance *gl_row;
        f32 y = origin_y + (f32)row * ch;
        f32 y_px = floorf(y);
        f32 ch_px = ceilf(ch);
        f32 cw_px = ceilf(cw);
        bool ligatures = style->ligatures;

        /* Resolve visible row -> scrollback or primary grid based on scroll_offset.
         *
         * This is the first of three resolves of the same row in one frame — the
         * decoration pass does its own below, and img_link's badge scan a third —
         * and a cold scrollback line is decompressed on each. Only DIRTY rows pay
         * it: in steady state the row caches replay, and it is a scrolling frame
         * that pays, because a scroll marks the whole screen dirty.
         *
         * The resolves stay separate because terminal_visible_row() hands back a
         * SCRATCH pointer that the next call invalidates, so sharing would mean
         * resolving every visible row into a caller-owned buffer up front and
         * restructuring three separate row loops to read it. */
        i32 row_len = 0;
        const Cell *row_cells = terminal_visible_row(term, row, &row_len);

        /* BiDi: if enabled and row has RTL content, produce visual-order cells
         * in a stack buffer (BIDI_MAX_COLS = 1024), avoiding allocation. */
        Cell bidi_visual[1024];
        Cell blank_cell = { .codepoint = ' ', .attr = term->cursor_attr };
        /* The gate is app_term_row_bidi_map's, spelled out here because this is
         * the one caller that also needs the reordered CELLS rather than only the
         * maps. The two must stay in step. */
        bool use_bidi = term->bidi_enabled &&
                        term->cols <= 1024 &&
                        row_cells && row_len >= term->cols &&
                        bidi_line_has_rtl(row_cells, term->cols);
        if (use_bidi) {
            bidi_reorder_line(row_cells, term->cols, bidi_visual, NULL);
            bidi_shape_arabic(bidi_visual, term->cols);
        }

        /* The run the ligature candidates are taken from (lig_run_fill). Only
         * the direct path shapes; a worker snapshot looks its answers up. */
        bool lig_direct = ligatures && !style->ligature && style->font;
        const Cell *lig_src = use_bidi ? bidi_visual : row_cells;
        i32 lig_src_len = use_bidi ? term->cols : (row_cells ? row_len : 0);
        FontLigatureRun lrun;
        i32 lrun_start = 0, lrun_end = 0;
        bool lrun_cut = false;

        for (i32 col = 0; col < term->cols; col++) {
            const Cell *cell;
            if (use_bidi) {
                cell = &bidi_visual[col];
            } else if (row_cells && col < row_len) {
                cell = &row_cells[col];
            } else {
                cell = &blank_cell;
            }
            u32 fg_val = cell->attr.fg;
            u32 bg_val = cell->attr.bg;
            bool has_inverse = (cell->attr.flags & ATTR_INVERSE) != 0;
            f32 x = origin_x + (f32)col * cw;
            f32 x_px = floorf(x);

            /* --- bg rect (color_resolve LUT) --- */
            bool emit_bg = has_inverse ||
                           IS_TRUECOLOR(bg_val) || bg_val != BG_DEFAULT;
            /* The colour this cell's text sits ON, computed outside the rect
             * branch because the glyph instance carries it too (min-contrast,
             * blend correction — see GlyphInstance in renderer.h). When no rect is
             * emitted the cell shows the pane's clear colour, which resolves from
             * the same default entry, so this is right in both branches.
             *
             * The opacity and background-image adjustments below are deliberately
             * NOT folded in: those are compositing against the desktop, while what
             * the shader needs is the colour a reader's eye judges the glyph
             * against. Folding them in would make a translucent window change its
             * own text contrast. */
            Color cell_bg = has_inverse
                ? cache_color_resolve(c, fg_val, FG_DEFAULT)
                : cache_color_resolve(c, bg_val, BG_DEFAULT);
            if (emit_bg) {
                Color bg = cell_bg;
                /* When a background image is active, reduce cell bg opacity
                 * so the wallpaper shows through. */
                if (style->wallpaper) {
                    bg.a *= (1.0f - style->wallpaper_opacity);
                }
                /* When window opacity < 1.0, reduce cell bg alpha so the
                 * desktop shows through. Overlays draw their own opaque rects.
                 */
                if (style->opacity < 1.0f) {
                    bg.a *= style->opacity;
                }
                bg = term_style_linear(style,bg);
                u32 key = has_inverse ? fg_val : bg_val;
                if (!IS_TRUECOLOR(key) && key >= 256)
                    key = has_inverse ? FG_DEFAULT : BG_DEFAULT;
                if (!term_row_append_background(c, row, &bg_n,
                                                &bg_key, key, x_px, y_px, cw_px, ch_px, bg)) continue;
            }

            /* --- glyph (color_resolve LUT) --- */
            if (cell->codepoint > 32 && !(cell->attr.flags & ATTR_WDUMMY)) {
                Color fg = has_inverse
                    ? cache_color_resolve(c, bg_val, BG_DEFAULT)
                    : cache_color_resolve(c, fg_val, FG_DEFAULT);
                if ((cell->attr.flags & ATTR_BOLD) && style->bold_bright &&
                    !IS_TRUECOLOR(fg_val) && fg_val < 8 && !has_inverse) {
                    /* The bright half of the palette is the terminal's too. */
                    fg = cache_color_resolve(c, fg_val + 8, FG_DEFAULT);
                }
                if (cell->attr.flags & ATTR_DIM) {
                    /* sRGB-space fudge factor: multiplying sRGB by 0.797 is
                     * perceptually equivalent to multiplying linear by 0.6.
                     * The conversion to linear happens at the CPU → GPU boundary
                     * (see renderer.h); keeping the fudge here avoids a
                     * round-trip srgb→lin→*0.6→srgb per cell. */
                    fg.r *= 0.797f; fg.g *= 0.797f; fg.b *= 0.797f;
                }
                /* The prompt while it is translated: last, so that it mixes
                 * with the colour the cell would really have had. */
                if (style->fx.phase && translate_fx_paint_has(&style->fx, row, col)) {
                    TranslateFxColor fc = translate_fx_paint_cell(&style->fx, col,
                        (TranslateFxColor){fg.r, fg.g, fg.b, fg.a},
                        (TranslateFxColor){cell_bg.r, cell_bg.g, cell_bg.b, cell_bg.a});
                    fg = (Color){fc.r, fc.g, fc.b, fc.a};
                }

                /* --- Ligature lookahead --- Gated on lig_can_start (punctuation
                 * openers only) so plain words never touch the ligature cache.
                 * Runs are gathered from the SAME visible-row source the glyphs
                 * render from (row_cells / bidi_visual): terminal_cell_at reads
                 * the LIVE grid, which is the wrong row while scrolled back into
                 * scrollback. Try longest match first (4, 3, 2 chars). */
                i32 lig_skip = 0;
                /* A ligature is drawn from the regular face: a bold or italic
                 * run keeps its letters in their own style instead. */
                if (ligatures && lig_can_start(cell->codepoint) &&
                    !(cell->attr.flags & (ATTR_BOLD | ATTR_ITALIC))) {
                    i32 max_look = 4;
                    if (col + max_look > term->cols) max_look = term->cols - col;
                    if (lig_direct && col < lig_src_len &&
                        (col >= lrun_end || (lrun_cut && col + max_look > lrun_end)))
                        lrun_start = lig_run_fill(&lrun, lig_src, lig_src_len, col,
                                                  &lrun_end, &lrun_cut);
                    for (i32 try_len = max_look; try_len >= 2; try_len--) {
                        /* Gather codepoints for the run, verify all are same
                         * fg/bg/attr (ligature should only apply to uniform runs) */
                        u32 run_cps[4];
                        bool uniform = true;
                        for (i32 k = 0; k < try_len; k++) {
                            const Cell *nc = NULL;
                            if (use_bidi)           nc = &bidi_visual[col + k];
                            else if (row_cells && col + k < row_len)
                                                    nc = &row_cells[col + k];
                            if (!nc || nc->codepoint < 33 || nc->codepoint >= 127 ||
                                (nc->attr.flags & (ATTR_WDUMMY | ATTR_WIDE))) {
                                uniform = false; break;
                            }
                            /* Ensure all cells share the same foreground.
                             *
                             * The raw fg/bg pair is not the whole answer: the
                             * colour actually drawn is that pair AFTER inverse
                             * swaps it, bold brightens it (bold_is_bright) and dim
                             * scales it — see the block above. A ligature is ONE
                             * instance carrying the FIRST cell's resolved colour,
                             * so a run that changes any of those mid-way would be
                             * painted in the wrong colour from that cell on.
                             * Attributes that pass 3 draws per cell (underline,
                             * strike) are deliberately not in the mask: they are
                             * emitted per cell either way. */
                            if (k > 0 && (nc->attr.fg != fg_val ||
                                          nc->attr.bg != bg_val ||
                                          ((nc->attr.flags ^ cell->attr.flags) &
                                           (ATTR_INVERSE | ATTR_BOLD | ATTR_DIM |
                                           ATTR_ITALIC)))) {
                                uniform = false; break;
                            }
                            run_cps[k] = nc->codepoint;
                        }
                        if (!uniform) continue;

                        FontGlyphRect lrect;
                        i32 lig_w = lig_direct
                            ? font_check_ligature_run(style->font, &lrun, col - lrun_start,
                                                      try_len, &lrect)
                            : term_style_ligature(style, run_cps, try_len, &lrect);
                        if (lig_w > 0) {
                            /* Emit ONE wide glyph instance for the ligature */
                            if (!term_render_cache_reserve_glyphs(c, row, gn + 1)) break;
                            gl_row = c->glyph_rows[row];
                            GlyphInstance *gi = &gl_row[gn++];
                            gi->x = x_px;
                            gi->y = y_px;
                            gi->ax0 = lrect.x0; gi->ay0 = lrect.y0;
                            gi->ax1 = lrect.x1; gi->ay1 = lrect.y1;
                            glyph_set_fg(gi, fg, 1.0f);
                            glyph_set_bg(gi, cell_bg);
                            gi->tex_kind = GLYPH_TEX_ALPHA;
                            gi->w_cells = (u8)(lig_w > 255 ? 255 : lig_w);
                            gi->_pad[0] = gi->_pad[1] = 0;
                            lig_skip = lig_w - 1;
                            break;
                        }
                    }
                }

                if (lig_skip > 0) {
                    /* Skip the remaining cells of the ligature (bg already emitted
                     * individually above; continue the loop to process bg for the
                     * next cells but the glyph part was handled as the wide glyph). */
                    for (i32 sk = 1; sk <= lig_skip && (col + sk) < term->cols; sk++) {
                        /* Same visible-row source as the run gather above. */
                        const Cell *sc = NULL;
                        if (use_bidi)           sc = &bidi_visual[col + sk];
                        else if (row_cells && col + sk < row_len)
                                                sc = &row_cells[col + sk];
                        if (!sc) continue;
                        u32 s_fg = sc->attr.fg;
                        u32 s_bg = sc->attr.bg;
                        bool s_inv = (sc->attr.flags & ATTR_INVERSE) != 0;
                        f32 sx = origin_x + (f32)(col + sk) * cw;
                        f32 sx_px = floorf(sx);
                        bool s_emit_bg = s_inv ||
                                         IS_TRUECOLOR(s_bg) || s_bg != BG_DEFAULT;
                        if (s_emit_bg) {
                            Color sbg = s_inv
                                ? cache_color_resolve(c, s_fg, FG_DEFAULT)
                                : cache_color_resolve(c, s_bg, BG_DEFAULT);
                            if (style->wallpaper) {
                                sbg.a *= (1.0f - style->wallpaper_opacity);
                            }
                            if (style->opacity < 1.0f) {
                                sbg.a *= style->opacity;
                            }
                            sbg = term_style_linear(style,sbg);
                            u32 key = s_inv ? s_fg : s_bg;
                            if (!IS_TRUECOLOR(key) && key >= 256)
                                key = s_inv ? FG_DEFAULT : BG_DEFAULT;
                            if (!term_row_append_background(c, row, &bg_n,
                                                            &bg_key, key, sx_px, y_px, cw_px, ch_px, sbg)) continue;
                        }
                    }
                    col += lig_skip; /* skip processed cells */
                    continue;
                }

                /* Normal single-glyph path */
                /* A bold or italic cell asks for its styled glyph, an icon
                 * with room after it for its two-cell drawing. */
                const Cell *gnext = NULL;
                if (col + 1 < term->cols) {
                    if (use_bidi) gnext = &bidi_visual[col + 1];
                    else if (row_cells && col + 1 < row_len) gnext = &row_cells[col + 1];
                    else gnext = &blank_cell;
                }
                u32 gkey = term_cell_glyph_key(cell, gnext);
                FontGlyphRect grect;
                bool got_glyph = term_style_glyph(style, gkey, &grect);
                if (got_glyph) {
                    if (!term_render_cache_reserve_glyphs(c, row, gn + 1)) continue;
                    gl_row = c->glyph_rows[row];
                    GlyphInstance *gi = &gl_row[gn++];
                    gi->x = x_px;
                    gi->y = y_px;
                    /* The rect says which atlas it lives in — the RGBA colour
                     * atlas for an emoji, the R8 alpha atlas for everything
                     * else — and both backends read the same kind value; the
                     * image path is the third kind and never appears in a grid
                     * row. Sampling the alpha atlas for an emoji would read R8
                     * texels as if they were RGBA: a tinted slab of whatever
                     * glyph sits at those coordinates. The rect is in TEXELS,
                     * which is what lets a cached row survive an atlas grow. */
                    bool is_color = (grect.kind == FONT_GLYPH_COLOR);
                    gi->tex_kind = grect.kind==FONT_GLYPH_SEED ? GLYPH_TEX_SEED : (is_color ? GLYPH_TEX_COLOR : GLYPH_TEX_ALPHA);
                    gi->ax0 = grect.x0;
                    gi->ay0 = grect.y0;
                    gi->ax1 = grect.x1;
                    gi->ay1 = grect.y1;
                    glyph_set_fg(gi, fg, 1.0f);
                    glyph_set_bg(gi, cell_bg);
                    gi->_pad[0] = gi->_pad[1] = 0;
                    /* The shader's colour branch multiplies the texel by fg (that
                     * is how white masks get tinted). An emoji is already full
                     * colour, so it passes white — otherwise it takes on the
                     * cell's foreground and e.g. a flag in yellow text goes
                     * yellow. */
                    if (is_color && term_style_emoji(style,gkey)) {
                        gi->fg[0] = gi->fg[1] = gi->fg[2] = 255;
                    }
                    /* Two-cell quad for the glyphs whose atlas raster is two
                     * cells — a colour emoji of two columns, CJK, fullwidth.
                     * ATTR_WIDE is the grid's own record of that, set by
                     * put_char from the same width the raster box is sized by;
                     * a one-column emoji is rastered into one cell. */
                    gi->w_cells = ((cell->attr.flags & ATTR_WIDE) ||
                                   (gkey & FONT_ICON_WIDE)) ? 2 : 1;
                }
            }
        }
        c->bg_counts[row]    = bg_n;
        c->glyph_counts[row] = gn;
    }

/* Only published UI caches need row versions; worker output is immutable and
 * acquires its versions after snapshot validation on the main thread. */
static void term_render_cache_versions_ensure(TermRenderCache *c) {
#ifdef USE_METAL
    if (c->bg_versions || c->cached_rows<=0) return;
    usize bytes;
    if (!checked_mul_usize((usize)c->cached_rows,2u*sizeof(u64),&bytes)) return;
    c->bg_versions=calloc(1,bytes);
    if (c->bg_versions) c->glyph_versions=c->bg_versions+c->cached_rows;
#else
    /* GL's orphaned storage is fully uploaded; only its batch revisions are
     * useful, so it does not retain unused per-row metadata. */
    (void)c;
#endif
}
static bool term_row_bytes_equal(const void *a,i32 an,const void *b,i32 bn,usize stride) {
    return an==bn && (!an || (a && b && !memcmp(a,b,(usize)an*stride)));
}
static void term_render_cache_note_row(TermRenderCache *c,i32 row,bool bg,bool glyph) {
    c->content_revision++;
    if (bg) {
        if (++c->bg_revision==0) {
            c->bg_revision=1;renderer_retained_release(&c->gpu_backgrounds);
        }
        if (c->bg_versions) c->bg_versions[row]=c->bg_revision;
    }
    if (glyph) {
        if (++c->glyph_revision==0) {
            c->glyph_revision=1;renderer_retained_release(&c->gpu_glyphs);
        }
        if (c->glyph_versions) c->glyph_versions[row]=c->glyph_revision;
    }
}
#define TERM_ROW_COMPARE_LIMIT 8
static bool term_render_cache_compare_sparse(const Terminal *term,bool full) {
    if (full) return false;
    i32 dirty=0;
    for (i32 row=0;row<term->rows;row++)
        if (term_row_dirty(term,row) && ++dirty>TERM_ROW_COMPARE_LIMIT) return false;
    return dirty<term->rows;
}
/* One spare row avoids copying an old viewport solely to compare it. Dense
 * rebuilds keep the in-place path and release this temporary extra capacity. */
static void term_render_cache_build_versioned_row(TermRenderCache *c,const TermRowStyle *style,
        Terminal *term,i32 row,f32 x,f32 y,f32 cw,f32 ch,bool compare) {
    if (!compare) {
        term_render_cache_build_row(c,style,term,row,x,y,cw,ch);
        term_render_cache_note_row(c,row,true,true);return;
    }
    RectInstance *old_bg=c->bg_rows[row];GlyphInstance *old_glyph=c->glyph_rows[row];
    i32 bn=c->bg_counts[row],gn=c->glyph_counts[row];
    i32 bc=c->bg_caps[row],gc=c->glyph_caps[row];
    /* Refusal before the swap leaves every original row untouched. */
    if (c->spare_bg_cap<bc) {
        RectInstance *next=LIU_TERM_ROW_REALLOC(c->spare_backgrounds,(usize)bc*sizeof(*next));
        if (!next) goto inplace;
        c->spare_backgrounds=next;c->spare_bg_cap=bc;
    }
    if (c->spare_glyph_cap<gc) {
        GlyphInstance *next=LIU_TERM_ROW_REALLOC(c->spare_glyphs,(usize)gc*sizeof(*next));
        if (!next) goto inplace;
        c->spare_glyphs=next;c->spare_glyph_cap=gc;
    }
    c->bg_rows[row]=c->spare_backgrounds;c->bg_caps[row]=c->spare_bg_cap;
    c->glyph_rows[row]=c->spare_glyphs;c->glyph_caps[row]=c->spare_glyph_cap;
    c->spare_backgrounds=old_bg;c->spare_bg_cap=bc;
    c->spare_glyphs=old_glyph;c->spare_glyph_cap=gc;
    term_render_cache_build_row(c,style,term,row,x,y,cw,ch);
    if (c->row_allocation_failed) {
        /* A growing replacement can fail after the swap. Free it and retry in
         * the original storage instead of publishing a spare-induced loss. */
        free(c->bg_rows[row]);free(c->glyph_rows[row]);
        c->bg_rows[row]=old_bg;c->bg_caps[row]=bc;c->bg_counts[row]=bn;
        c->glyph_rows[row]=old_glyph;c->glyph_caps[row]=gc;c->glyph_counts[row]=gn;
        c->spare_backgrounds=NULL;c->spare_glyphs=NULL;
        c->spare_bg_cap=0;c->spare_glyph_cap=0;
        goto inplace;
    }
    term_render_cache_note_row(c,row,
        !term_row_bytes_equal(old_bg,bn,c->bg_rows[row],c->bg_counts[row],sizeof(RectInstance)),
        !term_row_bytes_equal(old_glyph,gn,c->glyph_rows[row],c->glyph_counts[row],sizeof(GlyphInstance)));
    return;
inplace:
    term_render_cache_spare_free(c);
    term_render_cache_build_row(c,style,term,row,x,y,cw,ch);
    term_render_cache_note_row(c,row,true,true);
}

#include "ui/terminal_prepare.h"


#endif
