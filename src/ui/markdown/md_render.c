/*
 * Liu - Markdown renderer implementation.
 *
 * Per-block dispatch. Soft-wraps inline runs at viewport's max_x, breaking
 * at the last whitespace before overflow. Heading scale via
 * renderer_set_ui_scale (1.6×, 1.4×, …). Callout palette derived from
 * theme->ansi[]. Inline images go through MdImageCache.
 *
 * No lifetime concerns: the doc owns all slices via its arena, and the
 * caller (filebrowser) keeps both the arena and the source bytes alive
 * across this call.
 */
#include "ui/markdown/md_render.h"
#include "ui/markdown/md_math.h"
#include "ui/markdown/md_diagram.h"
#include "ui/markdown/md_embed.h"
#include "ui/chrome_palette.h"
#include "ui/syntax.h"
#include "ui/icon.h"
#include "core/utf8.h"

#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

/* =========================================================================
 * Typographic scale
 *
 * One table drives the whole document's vertical rhythm. Two rules hold
 * everywhere below:
 *
 *   1. A block's own height never includes the space that separates it from
 *      the NEXT block — that space is `space_after`, added by the dispatcher.
 *      Heading top space is the exception (it belongs to the heading, so the
 *      gap collapses correctly after a preceding paragraph).
 *   2. Nothing places text at a raw pixel offset. Everything is `t->unit`,
 *      `t->lh` or a heading's own line box.
 * ========================================================================= */

/* Heading steps. `scale` multiplies the base advance (which is what actually
 * sets type size); `top`/`bottom` are in units of MdTypo.unit. The ramp is
 * deliberately steeper at H1/H2 and nearly flat at H4-H6: hierarchy should be
 * obvious in the first two levels and stop competing after that. */
typedef struct { f32 scale, top, bottom; } MdHeadStep;
static const MdHeadStep H_STEP[7] = {
    { 1.00f, 0.00f, 0.00f },   /* unused (level 0) */
    { 1.85f, 1.30f, 0.62f },   /* H1 */
    { 1.42f, 1.15f, 0.48f },   /* H2 */
    { 1.20f, 0.95f, 0.38f },   /* H3 */
    { 1.06f, 0.85f, 0.32f },   /* H4 */
    { 1.00f, 0.80f, 0.30f },   /* H5 */
    { 0.94f, 0.75f, 0.28f },   /* H6 */
};

/* Longest comfortable line, in characters. Beyond ~90 the eye loses the
 * start of the next line. Wider viewports get margins, not longer lines.
 */
#define MD_MEASURE_COLS 88.0f

/* Leading, as a multiple of the drawn glyph height. Body text needs real
 * air; headings are large enough that the same ratio would look loose. */
#define MD_BODY_LEADING 1.34f
#define MD_HEAD_LEADING 1.16f
#define MD_CODE_LEADING 1.20f

/* Drawn height of a glyph at advance `cw` — the renderer derives it from the
 * font's real atlas aspect, so layout has to use the same formula or the line
 * box and the ink disagree (see renderer_set_ui_scale). */
static f32 md_glyph_h(const Renderer *r, f32 cw) {
    f32 aspect = 2.0f;
    if (r && r->font.cell_width > 0.0f && r->font.cell_height > 0.0f)
        aspect = r->font.cell_height / r->font.cell_width;
    if (!isfinite(aspect) || aspect <= 0.5f || aspect > 8.0f) aspect = 2.0f;
    return roundf(cw * aspect);
}

/* Baseline offset inside a line box of height `lh` holding a glyph of height
 * `gh`: the glyph is vertically centred by the renderer, so the baseline is
 * the centring offset plus the font's own ascent fraction. Underlines and
 * strikethroughs hang off this, so they stay with the text when the leading
 * changes. */
static f32 md_baseline(const Renderer *r, f32 lh, f32 gh) {
    f32 frac = 0.78f;
    if (r && r->font.cell_height > 0.0f && r->font.ascent > 0.0f) {
        f32 f = r->font.ascent / r->font.cell_height;
        if (f > 0.5f && f < 0.95f) frac = f;
    }
    return (lh - gh) * 0.5f + gh * frac;
}

/* Whole cells of advance `cw` that fit in `px` pixels. A block nested far
 * enough into lists and quotes in a narrow window starts to the RIGHT of its
 * own right edge, so the width is negative — and a negative float converted
 * to an unsigned count is undefined, not zero. */
static u32 md_cells_fit(f32 px, f32 cw) {
    if (!(cw > 0.0f) || !(px > 0.0f)) return 0;
    f32 n = px / cw;
    return n < 65535.0f ? (u32)n : 65535u;
}

static MdTypo md_typo_make(const Renderer *r, f32 cw, f32 ch, f32 dpi) {
    MdTypo t;
    t.cw       = cw;
    t.gh       = md_glyph_h(r, cw);
    t.lh       = roundf(t.gh * MD_BODY_LEADING);
    if (t.lh < t.gh + 2.0f) t.lh = t.gh + 2.0f;
    t.unit     = ch;                       /* the caller's nominal line box */
    t.baseline = md_baseline(r, t.lh, t.gh);
    t.indent   = roundf(cw * 2.2f);
    t.radius   = roundf(dpi * 6.0f);
    t.hair     = fmaxf(1.0f, roundf(dpi * 0.5f));
    t.pad_x    = roundf(cw * 0.85f);
    t.pad_y    = roundf(ch * 0.42f);
    return t;
}

/* Line box for a heading at `level`, and the advance that sizes its glyphs. */
static void md_head_metrics(const MdRenderCtx *ctx, u8 level,
                            f32 *out_cw, f32 *out_lh) {
    f32 sc  = H_STEP[level <= 6 ? level : 6].scale;
    /* Integer advance so every heading glyph steps by a constant width
     * (renderer_push_glyph floors x; a fractional advance produces tracking
     * jitter of ±1 px between characters). */
    f32 hcw = roundf(ctx->_t.cw * sc);
    f32 hgh = md_glyph_h(ctx->r, hcw);
    f32 hlh = roundf(hgh * MD_HEAD_LEADING);
    if (hlh < hgh + 2.0f) hlh = hgh + 2.0f;
    *out_cw = hcw;
    *out_lh = hlh;
}

/* =========================================================================
 * Static palette
 * ========================================================================= */

static Color callout_color(const Theme *t, MdCalloutKind k) {
    switch (k) {
    case MD_CALLOUT_INFO:     return t->ansi[14];
    case MD_CALLOUT_TIP:
    case MD_CALLOUT_SUCCESS:  return t->ansi[10];
    case MD_CALLOUT_WARNING:  return t->ansi[11];
    case MD_CALLOUT_DANGER:
    case MD_CALLOUT_FAILURE:
    case MD_CALLOUT_BUG:      return t->ansi[9];
    case MD_CALLOUT_QUESTION:
    case MD_CALLOUT_EXAMPLE:  return t->ansi[13];
    case MD_CALLOUT_QUOTE:
    case MD_CALLOUT_ABSTRACT: return t->ansi[8];
    case MD_CALLOUT_TODO:     return t->ansi[14];
    case MD_CALLOUT_NOTE:
    default:                  return t->ansi[12];
    }
}

static const char *callout_glyph(MdCalloutKind k) {
    switch (k) {
    case MD_CALLOUT_INFO:     return "i";
    case MD_CALLOUT_TIP:      return "*";
    case MD_CALLOUT_SUCCESS:  return "+";
    case MD_CALLOUT_WARNING:  return "!";
    case MD_CALLOUT_DANGER:
    case MD_CALLOUT_FAILURE:  return "x";
    case MD_CALLOUT_QUESTION: return "?";
    case MD_CALLOUT_EXAMPLE:  return ">";
    case MD_CALLOUT_QUOTE:    return "\"";
    case MD_CALLOUT_ABSTRACT: return "=";
    case MD_CALLOUT_TODO:     return "-";
    case MD_CALLOUT_BUG:      return "B";
    case MD_CALLOUT_NOTE:
    default:                  return "i";
    }
}

static Color color_with_alpha(Color c, f32 a) { c.a = a; return c; }

static Color color_dim(Color c, f32 amount) {
    Color o = c;
    o.r = c.r * amount;
    o.g = c.g * amount;
    o.b = c.b * amount;
    return o;
}

/* Push `c` toward higher contrast against `bg` by `amount` (0..1). On dark
 * backgrounds it lightens; on light backgrounds it darkens. Lightening
 * toward white regardless would push bold text into the background of a
 * light theme. */
static Color color_emphasize(Color c, Color bg, f32 amount) {
    f32 bg_lum = chrome_luminance(bg);
    Color o = c;
    if (bg_lum > 0.5f) {
        /* Light bg → darken toward black */
        o.r = c.r * (1.0f - amount);
        o.g = c.g * (1.0f - amount);
        o.b = c.b * (1.0f - amount);
    } else {
        /* Dark bg → lighten toward white */
        o.r = fminf(1.0f, c.r + (1.0f - c.r) * amount);
        o.g = fminf(1.0f, c.g + (1.0f - c.g) * amount);
        o.b = fminf(1.0f, c.b + (1.0f - c.b) * amount);
    }
    o.a = c.a;
    return o;
}

/* Returns the contrast ratio between two colors using WCAG luminance. */
static f32 color_contrast_ratio(Color a, Color b) {
    f32 la = chrome_luminance(a);
    f32 lb = chrome_luminance(b);
    f32 hi = la > lb ? la : lb;
    f32 lo = la > lb ? lb : la;
    return (hi + 0.05f) / (lo + 0.05f);
}

/* Blend `fg` toward whichever of black/white reads on `bg` until the
 * WCAG contrast ratio reaches at least `min_ratio`. Returns `fg` unchanged
 * if it already passes. */
static Color color_ensure_contrast(Color fg, Color bg, f32 min_ratio) {
    if (color_contrast_ratio(fg, bg) >= min_ratio) return fg;
    Color anchor = chrome_legible_on(bg);
    /* Bisect: blend amounts 0.25, 0.5, 0.75, 1.0 until passing or fully
     * collapsed onto the anchor. Cheap (≤4 steps); the optimum does not
     * matter, just "good enough". */
    f32 steps[] = {0.25f, 0.5f, 0.75f, 1.0f};
    for (u32 i = 0; i < sizeof(steps)/sizeof(steps[0]); i++) {
        f32 t = steps[i];
        Color m = {
            fg.r + (anchor.r - fg.r) * t,
            fg.g + (anchor.g - fg.g) * t,
            fg.b + (anchor.b - fg.b) * t,
            fg.a
        };
        if (color_contrast_ratio(m, bg) >= min_ratio) return m;
    }
    return anchor;
}

/* =========================================================================
 * Inline render — codepoint stream + soft wrap.
 *
 * Walks an MdInline run and emits glyphs at (cx, cy). Soft-wraps when
 * codepoint advance would exceed `max_x`. Returns final cy after layout.
 * All inline kinds map down to a sequence of (codepoint, color) emits with
 * optional underline / strikethrough rects.
 * ========================================================================= */

typedef struct {
    Renderer    *r;
    const Theme *theme;
    Color fg, fg_dim, fg_bright;
    Color link, code_bg, code_fg;
    Color tag_fg, tag_bg;
    Color hl_bg;            /* ==highlight== background */
    /* cw sets the type size; `ch` is the LINE BOX (leading included) that the
     * renderer centres the glyph inside, and `gh` is the glyph's real drawn
     * height. Decorations (underline, strike, pills) key off `base` — the
     * baseline's offset within the line box — so they follow the text when the
     * leading changes instead of drifting toward the box edge. */
    f32 cw, ch;
    f32 gh, base;
    f32 x_left, max_x;       /* bounds of one line */
    f32 line_y;
    f32 cx;
    f32 y_top, y_bottom;     /* viewport y-clip — emits outside are dropped */
    bool measure_only;       /* skip push_glyph / draw_rect; just advance y */

    /* Active link tracking. ir_link_begin/end bracket the inline emission of
     * a clickable run; ir_newline flushes the in-progress segment and
     * restarts on the next line so a soft-wrapped link records one rect per
     * visual line. NULL link_buf disables collection entirely. */
    MdLinkRect *link_buf;
    u32         link_cap;
    u32        *link_count;
    bool        link_active;
    const u8   *link_url;
    u32         link_url_len;
    const u8   *link_anchor;      /* "#Heading" / "^block-id" body, or NULL */
    u32         link_anchor_len;
    u8          link_anchor_is_block;
    u8          link_kind;        /* MdLinkRectKind */
    f32         link_seg_start_cx;
    f32         link_seg_start_line_y;
    /* Nesting depth of the link whose TEXT is being emitted. Unlike
     * link_active it is kept in measure passes too, so a run that lays itself
     * out differently inside a link measures the way it draws. */
    u8          in_link;

    /* Optional per-glyph rect collector for text selection (mirrors the
     * MdRenderCtx fields; wired in ir_make on the visible pass only). */
    MdGlyphRect *glyph_buf;
    u32          glyph_cap;
    u32         *glyph_count;
    /* MD_GLYPH_* for the NEXT glyph recorded, then cleared: "this row only
     * continues the one above" after a soft wrap, "a space stands before
     * this" for an item's text after its bullet. */
    u8           glyph_flags;

    /* Source span for glyph src_off tagging; cur_src_off is updated per byte
     * by ir_emit_bytes and read by mr_push_glyph. */
    const u8   *src_base;
    u32         src_len;
    u32         cur_src_off;

    /* Tallest box emitted on the current visual line (≥ ch). ir_newline
     * advances by this so a line carrying tall inline math doesn't overlap. */
    f32         line_max_h;
} InlineRender;

static inline void ir_link_flush_segment(InlineRender *ir) {
    if (ir->measure_only || !ir->link_active || !ir->link_buf || !ir->link_count) return;
    if (ir->link_seg_start_line_y + ir->ch <= ir->y_top || ir->link_seg_start_line_y >= ir->y_bottom) return;
    if (ir->cx <= ir->link_seg_start_cx) return;
    if (*ir->link_count >= ir->link_cap) return;
    ir->link_buf[*ir->link_count] = (MdLinkRect){
        .x       = ir->link_seg_start_cx,
        .y       = ir->link_seg_start_line_y,
        .w       = ir->cx - ir->link_seg_start_cx,
        .h       = ir->ch,
        .url     = ir->link_url,
        .url_len = ir->link_url_len,
        .anchor          = ir->link_anchor,
        .anchor_len      = ir->link_anchor_len,
        .anchor_is_block = ir->link_anchor_is_block,
        .kind    = ir->link_kind,
    };
    (*ir->link_count)++;
}

/* Begin a click region. `anchor` is the wikilink/embed fragment the parser
 * already split off the path (empty for everything else); it rides along on
 * each emitted rect so the click handler never has to look at the raw target
 * text again. */
static inline void ir_link_begin_anchored(InlineRender *ir, MdSlice url, u8 kind,
                                          MdSlice anchor, bool anchor_is_block) {
    /* An empty url is still a link when there is an anchor: "[[#Heading]]"
     * points at a section of THIS document, and refusing it here left the
     * text unclickable. */
    if (!ir->link_buf || !ir->link_count || (url.len == 0 && anchor.len == 0)) {
        ir->link_active = false;
        return;
    }
    ir->link_active           = true;
    ir->link_url              = url.data;
    ir->link_url_len          = url.len;
    ir->link_anchor           = anchor.data;
    ir->link_anchor_len       = anchor.len;
    ir->link_anchor_is_block  = anchor_is_block ? 1u : 0u;
    ir->link_kind             = kind;
    ir->link_seg_start_cx     = ir->cx;
    ir->link_seg_start_line_y = ir->line_y;
}

static inline void ir_link_begin(InlineRender *ir, MdSlice url, u8 kind) {
    MdSlice none = { NULL, 0 };
    ir_link_begin_anchored(ir, url, kind, none, false);
}

static inline void ir_link_end(InlineRender *ir) {
    if (!ir->link_active) return;
    ir_link_flush_segment(ir);
    ir->link_active = false;
}

/* `fb_render_viewer()` draws the title bar after Markdown to cover any
 * partial top line. Individual glyphs and rectangles are not y-clipped
 * because clipping partial overlaps can make top-edge glyphs flicker
 * during smooth scrolling. */
/* Record a glyph in the selection list WITHOUT drawing it: the space a soft
 * wrap swallowed, a superscript the renderer drew at its own size, the start
 * of a table cell (cp 0). The list is what a copy is made from, so what the
 * text contains has to be in it whether or not a quad was pushed for it. The
 * pending flags ride on whichever glyph is recorded next and are spent. */
static inline void mr_note_glyph(InlineRender *ir, f32 x, f32 y, f32 w, u32 cp) {
    if (ir->measure_only) return;
    if (ir->glyph_buf && ir->glyph_count && *ir->glyph_count < ir->glyph_cap) {
        ir->glyph_buf[*ir->glyph_count] =
            (MdGlyphRect){ x, y, w, ir->ch, cp, ir->cur_src_off, ir->glyph_flags };
        (*ir->glyph_count)++;
    }
    ir->glyph_flags = 0;
}
static inline void mr_push_glyph(InlineRender *ir, f32 x, f32 y, u32 cp, Color fg) {
    if (ir->measure_only) return;
    renderer_push_glyph(ir->r, x, y, cp, fg);
    mr_note_glyph(ir, x, y, ir->cw, cp);
}
static inline void mr_draw_rect(InlineRender *ir, f32 x, f32 y, f32 w, f32 h, Color c) {
    if (ir->measure_only) return;
    renderer_draw_rect(ir->r, x, y, w, h, c);
}
/* Rounded variant for inline chrome (code pills, tags, chips). Queues into the
 * rrect batch, which the renderer flushes just before glyphs, so the text
 * always lands on top of its own pill. */
static inline void mr_draw_rrect(InlineRender *ir, f32 x, f32 y, f32 w, f32 h,
                                 Color c, f32 radius) {
    if (ir->measure_only) return;
    renderer_draw_rrect_simple(ir->r, x, y, w, h, c, radius);
}

/* The block renderers' side of mr_note_glyph: record only. `w` is the rect's
 * width (a tab's gap is several cells, a blank line's marker none). */
static inline void cr_note_glyph(const MdRenderCtx *ctx, f32 x, f32 y, f32 w,
                                 u32 cp, u8 flags) {
    if (ctx->measure_only) return;
    if (ctx->glyph_rects && ctx->glyph_rect_count &&
        *ctx->glyph_rect_count < ctx->glyph_rect_cap) {
        ctx->glyph_rects[*ctx->glyph_rect_count] =
            (MdGlyphRect){ x, y, w, ctx->_t.lh, cp, MD_GLYPH_NO_SRC, flags };
        (*ctx->glyph_rect_count)++;
    }
}
static inline void cr_push_glyph_f(const MdRenderCtx *ctx, f32 x, f32 y, u32 cp,
                                   Color fg, u8 flags) {
    if (ctx->measure_only) return;
    renderer_push_glyph(ctx->r, x, y, cp, fg);
    cr_note_glyph(ctx, x, y, ctx->_t.cw, cp, flags);
}
static inline void cr_push_glyph(const MdRenderCtx *ctx, f32 x, f32 y, u32 cp, Color fg) {
    cr_push_glyph_f(ctx, x, y, cp, fg, 0);
}
/* Furniture: drawn, never recorded (a callout's icon letter). A drag across
 * the document must not select it and a copy must not carry it. */
static inline void cr_push_chrome(const MdRenderCtx *ctx, f32 x, f32 y, u32 cp, Color fg) {
    if (ctx->measure_only) return;
    renderer_push_glyph(ctx->r, x, y, cp, fg);
}
static inline void cr_draw_rect(const MdRenderCtx *ctx, f32 x, f32 y, f32 w, f32 h, Color c) {
    if (ctx->measure_only) return;
    renderer_draw_rect(ctx->r, x, y, w, h, c);
}

/* Rounded variant — document-flow cards (callouts, code blocks, frontmatter)
 * use this for the modern rounded look. No drop shadow: these sit in the text
 * flow, not floating. Queues into the rrect batch, which flushes just before
 * glyphs, so card text still lands on top. */
static inline void cr_draw_rrect(const MdRenderCtx *ctx, f32 x, f32 y, f32 w, f32 h,
                                 Color c, f32 radius) {
    if (ctx->measure_only) return;
    renderer_draw_rrect_simple(ctx->r, x, y, w, h, c, radius);
}

/* Case-insensitive compare of a slice to a C string (ASCII). */
static bool slice_eq_ci_cstr(MdSlice s, const char *cstr) {
    u32 n = (u32)strlen(cstr);
    if (s.len != n) return false;
    for (u32 i = 0; i < n; i++) {
        u8 a = s.data[i], b = (u8)cstr[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return false;
    }
    return true;
}

static void ir_newline(InlineRender *ir) {
    if (ir->link_active) ir_link_flush_segment(ir);
    ir->cx = ir->x_left;
    ir->line_y += ir->line_max_h;     /* tallest box on the line (≥ ch) */
    ir->line_max_h = ir->ch;
    if (ir->link_active) {
        ir->link_seg_start_cx     = ir->cx;
        ir->link_seg_start_line_y = ir->line_y;
    }
    /* A break the document asked for: whatever comes next starts a line. */
    ir->glyph_flags &= (u8)~MD_GLYPH_WRAP;
}

/* The line is full: go on below. Same motion as ir_newline, but the DOCUMENT
 * has no break here — only this window's width does — and the next glyph says
 * so, which is what keeps a copy from breaking every wrapped line. */
static void ir_wrap(InlineRender *ir) {
    ir_newline(ir);
    ir->glyph_flags |= MD_GLYPH_WRAP;
}

/* Underline / strikethrough for one cell, positioned off the baseline so both
 * sit where the type actually is: the underline just below the descender
 * shoulder, the strike through the x-height middle. */
static void ir_decorate(InlineRender *ir, f32 x, f32 w, Color fg,
                        bool underline, bool strike) {
    if (!underline && !strike) return;
    f32 thick = fmaxf(1.0f, roundf(ir->gh * 0.055f));
    if (underline)
        mr_draw_rect(ir, x, ir->line_y + ir->base + roundf(ir->gh * 0.12f),
                     w, thick, color_with_alpha(fg, fg.a * 0.7f));
    if (strike)
        mr_draw_rect(ir, x, ir->line_y + ir->base - roundf(ir->gh * 0.22f),
                     w, thick, fg);
}

/* Walk a slice of UTF-8 bytes, emitting codepoints with the supplied color.
 * Word-aware soft wrap: at each word boundary it looks ahead and, if the whole
 * word would overflow the current line, moves it down first so the break lands
 * between words rather than mid-word. A single word longer than the line still
 * hard-breaks. (Cells are monospace, so a word's width is its codepoint count
 * times ir->cw — no glyph buffering / roll-back needed.) */
static void ir_emit_bytes(InlineRender *ir, MdSlice s, Color fg, bool with_underline,
                          bool with_strike) {
    if (!ir->measure_only) {
        /* The warm pass pre-rasterizes non-ASCII glyphs; font_warm no-ops on
         * cp < 127, so skip the full utf8 walk entirely for pure-ASCII runs
         * (any cp >= 127 always has a byte with the high bit set). */
        bool has_non_ascii = false;
        for (u32 k = 0; k < s.len; k++) { if (s.data[k] & 0x80) { has_non_ascii = true; break; } }
        if (has_non_ascii)
            font_warm_text_glyphs_n(&ir->r->font, (const char *)s.data, s.len);
    }
    f32 step = ir->cw;
    bool at_word_start = true;
    /* Map this slice back to a source offset so emitted glyphs carry src_off.
     * Synthetic slices (literal "[^", alt text, …) fall outside the span and
     * get MD_GLYPH_NO_SRC. */
    bool s_in_src = false;
    u32 s_base_off = MD_GLYPH_NO_SRC;
    if (ir->src_base && s.data) {
        uintptr_t base = (uintptr_t)(const void *)ir->src_base;
        uintptr_t data = (uintptr_t)(const void *)s.data;
        if (data >= base) {
            usize off = (usize)(data - base);
            if (off <= ir->src_len && (usize)s.len <= (usize)ir->src_len - off) {
                s_in_src = true;
                s_base_off = (u32)off;
            }
        }
    }
    u32 i = 0;
    while (i < s.len) {
        u32 cp = 0;
        u32 n = utf8_decode(s.data + i, s.len - i, &cp);
        if (n == 0) { i++; continue; }
        ir->cur_src_off = s_in_src ? s_base_off + i : MD_GLYPH_NO_SRC;

        /* Fold the joiners that follow into one grapheme and give it the cells
         * it actually draws in — the terminal grid's rules, applied to the
         * reader. Without this ❤️ drew a thin ❤ plus a blank, and every emoji
         * or ideograph overlapped the character after it (the renderer emits a
         * two-cell quad for both). */
        i32 gcols = 1;
        u32 gbytes = utf8_next_grapheme(s.data + i, s.len - i, &cp, &gcols);
        if (gbytes > 0) n = gbytes;
        f32 adv = (gcols == 2) ? step * 2.0f : step;

        /* A '\n' still inside a text run is a CommonMark SOFT break: the parser
         * already split every HARD break out as MD_INLINE_LINEBREAK, so what is
         * left reflows as an ordinary space. Breaking the line here instead is
         * what made the reader preserve the author's source wrapping — a
         * paragraph hand-wrapped at 80 columns stayed 80 columns wide in a
         * window twice that size. */
        if (cp == '\n') {
            cp = ' ';
            /* A soft break also eats the next line's indentation. CommonMark
             * strips leading whitespace from a continuation line, and a list
             * item's continuation carries the item's own indent inside its
             * source slice — drawn literally, that opened a gap in the middle
             * of the wrapped sentence. */
            while (i + n < s.len && (s.data[i + n] == ' ' || s.data[i + n] == '\t')) n++;
        }

        if (cp == ' ' || cp == '\t') {
            /* Break at the space if drawing it would overflow; otherwise
             * advance past it (underline/strike continue across it). */
            if (ir->cx + step > ir->max_x) {
                /* The space is not drawn — the wrap stands in for it on
                 * screen — but it is still a space in the text. */
                mr_note_glyph(ir, ir->cx, ir->line_y, 0.0f, ' ');
                ir_wrap(ir);
            } else {
                if (cp >= 32) mr_push_glyph(ir, ir->cx, ir->line_y, cp, fg);
                ir_decorate(ir, ir->cx, step, fg, with_underline, with_strike);
                ir->cx += step;
            }
            i += n;
            at_word_start = true;
            continue;
        }

        /* Word character. At the start of a word, look ahead: if the whole word
         * would overflow the current line (and the cursor is not already at the
         * line start), move it down so the break falls between words, not
         * mid-word. */
        if (at_word_start) {
            u32 word_cps = 0;
            for (u32 j = i; j < s.len; ) {
                u32 wcp = 0;
                u32 wn = utf8_decode(s.data + j, s.len - j, &wcp);
                if (wn == 0) { j++; continue; }
                if (wcp == ' ' || wcp == '\t' || wcp == '\n') break;
                /* Count COLUMNS, not codepoints: a joiner takes none and an
                 * emoji takes two, so measuring per codepoint would wrap a
                 * word of emoji a line early (or late). */
                word_cps += (u32)utf8_char_width(wcp);
                j += wn;
            }
            if (ir->cx > ir->x_left &&
                ir->cx + (f32)word_cps * step > ir->max_x) {
                ir_wrap(ir);
            }
            at_word_start = false;
        }

        /* Mid-word hard break: only for a word wider than the whole line. */
        if (ir->cx + adv > ir->max_x && ir->cx > ir->x_left) {
            ir_wrap(ir);
        }

        if (cp >= 32) mr_push_glyph(ir, ir->cx, ir->line_y, cp, fg);
        ir_decorate(ir, ir->cx, adv, fg, with_underline, with_strike);
        ir->cx += adv;
        i += n;
    }
}

/* Does this embed target name a DOCUMENT (transclude it) rather than a file
 * the image/media pipeline handles? Wikilinks routinely omit the extension, so
 * "no extension" means a note. */
static bool embed_is_document(MdSlice s) {
    i32 dot = -1;
    for (i32 i = (i32)s.len - 1; i >= 0; i--) {
        if (s.data[i] == '.') { dot = i; break; }
        if (s.data[i] == '/') break;
    }
    if (dot < 0 || (u32)dot + 1 >= s.len) return true;   /* no extension = note */
    char ext[8]; u32 el = 0;
    for (u32 i = (u32)dot + 1; i < s.len && el < sizeof ext - 1; i++) {
        u8 c = s.data[i];
        if (c >= 'A' && c <= 'Z') c = (u8)(c - 'A' + 'a');
        ext[el++] = (char)c;
    }
    ext[el] = '\0';
    return strcmp(ext, "md") == 0 || strcmp(ext, "markdown") == 0 ||
           strcmp(ext, "txt") == 0;
}

/* Short media-type tag for an embed/link target by file extension, or NULL for
 * anything not a recognized media/document file (those keep the plain link
 * placeholder). Case-insensitive, no <strings.h> dependency. */
static const char *media_chip_label(MdSlice s) {
    i32 dot = -1;
    for (i32 i = (i32)s.len - 1; i >= 0; i--) {
        if (s.data[i] == '.') { dot = i; break; }
        if (s.data[i] == '/') break;
    }
    if (dot < 0 || (u32)dot + 1 >= s.len) return NULL;
    char ext[8]; u32 el = 0;
    for (u32 i = (u32)dot + 1; i < s.len && el < sizeof ext - 1; i++) {
        u8 c = s.data[i];
        if (c >= 'A' && c <= 'Z') c = (u8)(c - 'A' + 'a');
        ext[el++] = (char)c;
    }
    ext[el] = '\0';
    if (!strcmp(ext,"mp4")||!strcmp(ext,"mov")||!strcmp(ext,"mkv")||
        !strcmp(ext,"webm")||!strcmp(ext,"avi")||!strcmp(ext,"m4v")) return "VIDEO";
    if (!strcmp(ext,"mp3")||!strcmp(ext,"wav")||!strcmp(ext,"ogg")||
        !strcmp(ext,"flac")||!strcmp(ext,"m4a")||!strcmp(ext,"aac")) return "AUDIO";
    if (!strcmp(ext,"pdf")) return "PDF";
    return NULL;
}

/* Emit a sub/superscript run: the body at 0.72× cell size, top-aligned (sup)
 * or bottom-aligned (sub). Brackets a set_ui_scale flush, so it's heavier than
 * plain text — fine for the rare sub/sup. */
static void ir_emit_script(InlineRender *ir, MdSlice s, bool sup) {
    const f32 sc = 0.72f;
    f32 scw = ir->cw * sc;
    usize cells = utf8_display_cols(s.data, s.len);
    f32 span = (f32)cells * scw;
    if (ir->cx + span > ir->max_x && ir->cx > ir->x_left) ir_wrap(ir);
    if (!ir->measure_only && cells > 0) {
        renderer_flush_glyphs(ir->r);
        renderer_set_ui_scale(ir->r, ir->cw * sc, ir->ch * sc);
        f32 ytop = sup ? ir->line_y : ir->line_y + (ir->ch - ir->ch * sc);
        f32 cx = ir->cx;
        ir->cur_src_off = MD_GLYPH_NO_SRC;
        u32 i = 0;
        while (i < s.len) {
            u32 cp = 0; u32 nn = utf8_decode(s.data + i, s.len - i, &cp);
            if (!nn) { i++; continue; }
            if (cp >= 32) {
                renderer_push_glyph(ir->r, cx, ytop, cp, ir->fg);
                /* Recorded on the LINE's box, not on the raised one it is
                 * drawn in: the selection groups a row by its y, and a
                 * subscript is text of its line, so "H~2~O" copies as "H2O".
                 */
                mr_note_glyph(ir, cx, ir->line_y, scw, cp);
                cx += scw;
            }
            i += nn;
        }
        renderer_flush_glyphs(ir->r);
        renderer_set_ui_scale(ir->r, ir->cw, ir->ch);   /* restore base scale */
    }
    ir->cx += span;
}

/* Emit an inline run list with an inherited style (color + underline + strike).
 * Emphasis / link runs recurse into their `children` so nesting composes
 * (**bold *italic*** , [**x**](url)). Pre-B1 docs (flat runs, no children) fall
 * back to the run's `text` slice, so behavior is unchanged for them. */
static void ir_emit_runs(InlineRender *ir, const MdInline *runs, u32 n,
                         Color base_fg, bool underline, bool strike) {
    for (u32 i = 0; i < n; i++) {
        const MdInline *r = &runs[i];
        switch (r->kind) {
        case MD_INLINE_TEXT:
            ir_emit_bytes(ir, r->text, base_fg, underline, strike);
            break;
        case MD_INLINE_BOLD:
        case MD_INLINE_BOLD_ITALIC:
            if (r->child_count && r->children)
                ir_emit_runs(ir, r->children, r->child_count, ir->fg_bright, underline, strike);
            else
                ir_emit_bytes(ir, r->text, ir->fg_bright, underline, strike);
            break;
        case MD_INLINE_ITALIC:
            if (r->child_count && r->children)
                ir_emit_runs(ir, r->children, r->child_count, ir->fg_dim, underline, strike);
            else
                ir_emit_bytes(ir, r->text, ir->fg_dim, underline, strike);
            break;
        case MD_INLINE_STRIKETHROUGH:
            if (r->child_count && r->children)
                ir_emit_runs(ir, r->children, r->child_count, ir->fg_dim, underline, true);
            else
                ir_emit_bytes(ir, r->text, ir->fg_dim, underline, true);
            break;
        case MD_INLINE_HIGHLIGHT: {
            usize w_cells = utf8_display_cols(r->text.data, r->text.len);
            f32 span = (f32)w_cells * ir->cw;
            if (ir->cx + span > ir->max_x) ir_wrap(ir);
            /* Hug the ink rather than filling the full leading — a highlighter
             * that spans the whole line box reads as a selection, not as
             * marked-up text. */
            f32 hl_h = roundf(ir->gh * 0.86f);
            f32 hl_y = ir->line_y + ir->base - roundf(ir->gh * 0.66f);
            mr_draw_rect(ir, ir->cx - 1, hl_y, span + 2, hl_h, ir->hl_bg);
            if (r->child_count && r->children)
                ir_emit_runs(ir, r->children, r->child_count, base_fg, underline, strike);
            else
                ir_emit_bytes(ir, r->text, base_fg, underline, strike);
            break;
        }
        case MD_INLINE_CODE: {
            /* width = codepoint count * cw (rough but close enough) */
            usize bytes = r->text.len;
            usize w_cells = utf8_display_cols(r->text.data, bytes);
            f32 span = (f32)w_cells * ir->cw;
            if (ir->cx + span > ir->max_x) ir_wrap(ir);
            /* Rounded pill sized to the ink, not to the line box: a full-height
             * rectangle behind one word is the single loudest thing on a page
             * of prose. */
            f32 pill_h = roundf(ir->gh * 0.92f);
            f32 pill_y = ir->line_y + ir->base - roundf(ir->gh * 0.70f);
            f32 bleed  = roundf(ir->cw * 0.22f);
            mr_draw_rrect(ir, ir->cx - bleed, pill_y, span + 2.0f * bleed,
                          pill_h, ir->code_bg, roundf(pill_h * 0.30f));
            ir_emit_bytes(ir, r->text, ir->code_fg, false, false);
            break;
        }
        case MD_INLINE_LINK:
        case MD_INLINE_AUTOLINK:
        case MD_INLINE_WIKILINK: {
            MdSlice display = r->text.len ? r->text : r->url;
            if (r->kind == MD_INLINE_WIKILINK && r->alt.len) display = r->alt;
            else if (r->kind == MD_INLINE_WIKILINK)          display = r->url;
            /* An anchored link with no alias reads as "Note › Heading": the
             * anchor is part of what the link points at, and hiding it made
             * three links to three sections of one note look identical.
             *
             * "[[#Heading]]" has no note part, so it is just "Heading" — with
             * the separator suppressed, which otherwise rendered as a dangling
             * " › " with nothing in front of it. */
            bool same_file = (r->kind == MD_INLINE_WIKILINK && r->url.len == 0 &&
                              r->anchor.len > 0);
            bool show_anchor = (r->kind == MD_INLINE_WIKILINK &&
                                r->alt.len == 0 && r->anchor.len > 0);
            if (same_file && r->alt.len == 0) {
                display = r->anchor;
                show_anchor = false;
            }
            /* AUTOLINK is parsed via push_styled_run which stores the URL
             * in `text`, not `url` — both display and click target are the
             * same slice. */
            MdSlice click_url = (r->kind == MD_INLINE_AUTOLINK) ? r->text : r->url;
            u8 lk = (r->kind == MD_INLINE_WIKILINK) ? MD_LINKRECT_WIKILINK
                                                    : MD_LINKRECT_URL;
            ir_link_begin_anchored(ir, click_url, lk, r->anchor,
                                   r->anchor_is_block != 0);
            if (r->kind == MD_INLINE_LINK && r->child_count && r->children) {
                ir->in_link++;
                ir_emit_runs(ir, r->children, r->child_count, ir->link, true, strike);
                ir->in_link--;
            } else {
                ir_emit_bytes(ir, display, ir->link, true, false);
            }
            if (show_anchor) {
                const char *sep = r->anchor_is_block ? " \xc2\xb6 " : " \xe2\x80\xba ";
                MdSlice ss = { (const u8 *)sep, (u32)strlen(sep) };
                ir_emit_bytes(ir, ss, color_with_alpha(ir->link, 0.65f), true, false);
                ir_emit_bytes(ir, r->anchor, ir->link, true, false);
            }
            ir_link_end(ir);
            break;
        }
        case MD_INLINE_TAG: {
            usize w_cells = utf8_display_cols(r->text.data, r->text.len) + 1;
            f32 span = (f32)w_cells * ir->cw;
            if (ir->cx + span > ir->max_x) ir_wrap(ir);
            f32 tag_h = roundf(ir->gh * 0.92f);
            f32 tag_y = ir->line_y + ir->base - roundf(ir->gh * 0.70f);
            f32 tag_bleed = roundf(ir->cw * 0.30f);
            mr_draw_rrect(ir, ir->cx - tag_bleed, tag_y,
                          span + 2.0f * tag_bleed, tag_h,
                          ir->tag_bg, tag_h * 0.5f);
            /* Record a clickable tag rect (url = the tag name, no '#'). */
            ir_link_begin(ir, r->text, MD_LINKRECT_TAG);
            mr_push_glyph(ir, ir->cx, ir->line_y, '#', ir->tag_fg);
            ir->cx += ir->cw;
            ir_emit_bytes(ir, r->text, ir->tag_fg, false, false);
            ir_link_end(ir);
            break;
        }
        case MD_INLINE_LINEBREAK:
            ir_newline(ir);
            break;
        case MD_INLINE_IMAGE:
        case MD_INLINE_EMBED: {
            /* Media/document embeds (mp4/mp3/pdf/…) render as a labelled chip;
             * everything else keeps the plain clickable placeholder. Both are
             * treated at block level for sizing — this is the inline fallback
             * (e.g. inside a list item), and stays clickable to open the file. */
            MdSlice tgt = r->url.len ? r->url : r->alt;
            const char *mlabel = media_chip_label(tgt);
            /* Inside a link — a README badge, "[![Build](img)](https://ci)" —
             * the picture is the link's TEXT. The click target under it stays
             * the enclosing link's; opening one here would replace it, and the
             * badge would open its own image instead of where it points. */
            if (ir->in_link) {
                ir_emit_bytes(ir, r->alt.len ? r->alt : r->url, ir->link, true, false);
            } else if (mlabel) {
                /* basename of the target */
                const u8 *nm = tgt.data; u32 nl = tgt.len;
                for (i32 k = (i32)tgt.len - 1; k >= 0; k--)
                    if (tgt.data[k] == '/') { nm = tgt.data + k + 1; nl = tgt.len - (u32)(k + 1); break; }
                u32 lbl = (u32)strlen(mlabel);
                /* Cap the name so the chip fits one line (the pill background is
                 * a single fixed-width rect; a wrapped name would escape it). */
                u32 line_cells = md_cells_fit(ir->max_x - ir->x_left, ir->cw);
                if (line_cells < lbl + 4) line_cells = lbl + 4;
                u32 max_name = line_cells - lbl - 2;
                if (nl > max_name) nl = max_name;
                f32 span = (f32)(lbl + 1 + nl) * ir->cw;
                if (ir->cx + span > ir->max_x && ir->cx > ir->x_left) ir_wrap(ir);
                f32 mc_h = roundf(ir->gh * 0.96f);
                f32 mc_y = ir->line_y + ir->base - roundf(ir->gh * 0.72f);
                mr_draw_rrect(ir, ir->cx - 3, mc_y, span + 6, mc_h,
                              ir->tag_bg, roundf(mc_h * 0.32f));
                ir_link_begin_anchored(ir, tgt, MD_LINKRECT_EMBED, r->anchor,
                                       r->anchor_is_block != 0);
                MdSlice ls = { (const u8 *)mlabel, lbl };
                ir_emit_bytes(ir, ls, ir->tag_fg, false, false);
                ir->cx += ir->cw;   /* gap */
                ir->glyph_flags |= MD_GLYPH_GAP;
                MdSlice ns = { nm, nl };
                ir_emit_bytes(ir, ns, ir->fg, false, false);
                ir_link_end(ir);
            } else {
                ir_link_begin_anchored(ir, r->url,
                                       r->kind == MD_INLINE_IMAGE ? MD_LINKRECT_IMAGE
                                                                  : MD_LINKRECT_EMBED,
                                       r->anchor, r->anchor_is_block != 0);
                ir_emit_bytes(ir, r->alt.len ? r->alt : r->url, ir->link, true, false);
                ir_link_end(ir);
            }
            break;
        }
        case MD_INLINE_FOOTNOTE_REF: {
            /* Numbered by the parser (flags) → small superscript number;
             * an unresolved ref keeps the literal [^id]. */
            if (r->flags > 0) {
                char num[8];
                int nn = snprintf(num, sizeof num, "%u", (unsigned)r->flags);
                ir_emit_script(ir, (MdSlice){ (const u8 *)num, (u32)nn }, true);
            } else {
                ir_emit_bytes(ir, (MdSlice){ (const u8 *)"[^", 2 }, ir->fg_dim, false, false);
                ir_emit_bytes(ir, r->text, ir->fg_dim, false, false);
                ir_emit_bytes(ir, (MdSlice){ (const u8 *)"]",  1 }, ir->fg_dim, false, false);
            }
            break;
        }
        case MD_INLINE_HTML: {
            switch (r->flags) {
            case MD_HTML_BOLD:
                if (r->child_count && r->children)
                    ir_emit_runs(ir, r->children, r->child_count, ir->fg_bright, underline, strike);
                else ir_emit_bytes(ir, r->text, ir->fg_bright, underline, strike);
                break;
            case MD_HTML_ITALIC:
                if (r->child_count && r->children)
                    ir_emit_runs(ir, r->children, r->child_count, ir->fg_dim, underline, strike);
                else ir_emit_bytes(ir, r->text, ir->fg_dim, underline, strike);
                break;
            case MD_HTML_UNDERLINE:
                if (r->child_count && r->children)
                    ir_emit_runs(ir, r->children, r->child_count, base_fg, true, strike);
                else ir_emit_bytes(ir, r->text, base_fg, true, strike);
                break;
            case MD_HTML_MARK: {
                usize w_cells = utf8_display_cols(r->text.data, r->text.len);
                f32 span = (f32)w_cells * ir->cw;
                if (ir->cx + span > ir->max_x) ir_wrap(ir);
                f32 hl_h = roundf(ir->gh * 0.86f);
                f32 hl_y = ir->line_y + ir->base - roundf(ir->gh * 0.66f);
                mr_draw_rect(ir, ir->cx - 1, hl_y, span + 2, hl_h, ir->hl_bg);
                ir_emit_bytes(ir, r->text, base_fg, underline, strike);
                break;
            }
            case MD_HTML_KBD: {
                /* Key cap: a bordered chip, drawn as two rounded rects because
                 * the renderer has no stroked-rrect primitive. */
                usize w_cells = utf8_display_cols(r->text.data, r->text.len);
                f32 span = (f32)w_cells * ir->cw;
                f32 pad  = roundf(ir->cw * 0.35f);
                if (ir->cx + span + 2 * pad > ir->max_x) ir_wrap(ir);
                f32 cap_h = roundf(ir->gh * 0.98f);
                f32 cap_y = ir->line_y + ir->base - roundf(ir->gh * 0.74f);
                f32 rad   = roundf(cap_h * 0.26f);
                f32 bw    = fmaxf(1.0f, roundf(ir->cw * 0.09f));
                mr_draw_rrect(ir, ir->cx - pad, cap_y, span + 2 * pad, cap_h,
                              color_with_alpha(ir->fg, 0.38f), rad);
                mr_draw_rrect(ir, ir->cx - pad + bw, cap_y + bw,
                              span + 2 * pad - 2 * bw, cap_h - 2 * bw - bw,
                              ir->code_bg, fmaxf(0.0f, rad - bw));
                ir_emit_bytes(ir, r->text, ir->fg_bright, false, false);
                break;
            }
            default:
                if (r->child_count && r->children)
                    ir_emit_runs(ir, r->children, r->child_count, base_fg, underline, strike);
                else ir_emit_bytes(ir, r->text, base_fg, underline, strike);
                break;
            }
            break;
        }
        case MD_INLINE_SUBSCRIPT:   ir_emit_script(ir, r->text, false); break;
        case MD_INLINE_SUPERSCRIPT: ir_emit_script(ir, r->text, true);  break;
        case MD_INLINE_MATH: {
            /* Atomic box laid out by the native math engine, baseline-aligned to
             * the surrounding text. */
            f32 mw = 0, ma = 0, md_ = 0;
            md_math_measure(r->text.data, r->text.len, ir->cw, ir->ch, false, &mw, &ma, &md_);
            if (mw <= 0.0f) {                /* engine produced nothing → show raw */
                ir_emit_bytes(ir, r->text, ir->code_fg, false, false);
                break;
            }
            if (ir->cx + mw > ir->max_x && ir->cx > ir->x_left) ir_wrap(ir);
            if (!ir->measure_only) {
                f32 baseline = ir->line_y + 0.75f * ir->ch;   /* text baseline */
                md_math_render(ir->r, r->text.data, r->text.len, ir->cx, baseline,
                               ir->cw, ir->ch, false, ir->fg);
            }
            ir->cx += mw;
            f32 tot = ma + md_;
            if (tot > ir->line_max_h) ir->line_max_h = tot;
            break;
        }
        default:
            break;
        }
    }
}

/* Top-level inline emission: plain body color, no decoration inherited. */
static void ir_emit_inlines(InlineRender *ir, const MdInline *runs, u32 n) {
    ir_emit_runs(ir, runs, n, ir->fg, false, false);
}

/* =========================================================================
 * Block renderers
 * ========================================================================= */

/* `cw` is the type size, `ch` the line box the caller wants the text laid out
 * on (body: ctx->_t.lh; headings: their own). The glyph height and baseline
 * are derived from cw, never guessed. */
static InlineRender ir_make(MdRenderCtx *ctx, f32 line_y, f32 x, f32 max_x,
                            f32 cw, f32 ch) {
    InlineRender ir = {0};
    ir.gh      = md_glyph_h(ctx->r, cw);
    if (ch < ir.gh) ch = ir.gh;      /* never lay type on a box it can't fit */
    ir.base    = md_baseline(ctx->r, ch, ir.gh);
    ir.r       = ctx->r;
    ir.theme   = ctx->theme;
    ir.fg          = ctx->_fg;
    ir.fg_dim      = ctx->_fg_dim;
    ir.fg_bright   = ctx->_fg_bright;
    ir.link        = ctx->_link;
    ir.code_bg     = ctx->_code_bg;
    ir.code_fg     = ctx->_code_fg;
    ir.tag_fg      = ctx->_tag_fg;
    ir.tag_bg      = ctx->_tag_bg;
    ir.hl_bg       = color_with_alpha(ctx->theme->ansi[3], 0.32f);  /* yellow highlighter */
    ir.cw          = cw;
    ir.ch          = ch;
    ir.x_left      = x;
    ir.max_x       = max_x;
    ir.line_y      = line_y;
    ir.cx          = x;
    ir.measure_only = ctx->measure_only;
    ir.y_top        = ctx->y;
    ir.y_bottom     = ctx->y + ctx->h;
    ir.src_base     = ctx->src_base;
    ir.src_len      = ctx->src_len;
    ir.cur_src_off  = MD_GLYPH_NO_SRC;
    ir.line_max_h   = ch;
    /* Link collection: only emit rects from the visible pass — measure_only
     * runs would record duplicates (same link, off-screen y) and the caller
     * doesn't need those for hit testing. */
    if (!ctx->measure_only) {
        ir.link_buf   = ctx->link_rects;
        ir.link_cap   = ctx->link_rect_cap;
        ir.link_count = ctx->link_rect_count;
        ir.glyph_buf   = ctx->glyph_rects;
        ir.glyph_cap   = ctx->glyph_rect_cap;
        ir.glyph_count = ctx->glyph_rect_count;
    }
    return ir;
}

static f32 ir_finish(const InlineRender *ir) {
    return ir->cx > ir->x_left ? ir->line_y + ir->line_max_h : ir->line_y;
}

/* A footnote-definition paragraph begins with a footnote ref followed by a
 * ':' (e.g. "[^1]: the note text"). The line-leading position is what makes it
 * a definition rather than an inline reference — an inline ref is always
 * preceded by text, so inlines[0] is only a FOOTNOTE_REF for a definition. */
static bool para_is_footnote_def(const MdBlock *b) {
    return b && b->inlines && b->inline_count >= 2 &&
           b->inlines[0].kind == MD_INLINE_FOOTNOTE_REF &&
           b->inlines[1].kind == MD_INLINE_TEXT &&
           b->inlines[1].text.len >= 1 && b->inlines[1].text.data[0] == ':';
}

/* Render a footnote definition: dim body with a thin accent bar in the gutter,
 * visually setting the back-matter apart from the document body. */
static f32 render_footnote_def(const MdBlock *b, MdRenderCtx *ctx,
                               f32 x, f32 y, f32 max_x) {
    const MdTypo *t = &ctx->_t;
    f32 bar_w = fmaxf(1.5f, roundf(ctx->dpi));
    Color bar = color_with_alpha(ctx->_link, 0.45f);
    f32 start_y = y;
    InlineRender ir = ir_make(ctx, y, x + bar_w + t->pad_x, max_x, t->cw, t->lh);
    ir.fg = ctx->_muted;
    ir_emit_inlines(&ir, b->inlines, b->inline_count);
    f32 end_y = ir_finish(&ir);
    cr_draw_rect(ctx, x, start_y, bar_w, end_y - start_y, bar);
    return end_y + t->unit * 0.35f;
}

static f32 render_paragraph(const MdBlock *b, MdRenderCtx *ctx,
                             f32 x, f32 y, f32 max_x) {
    if (!ctx->in_nested_block && para_is_footnote_def(b))
        return render_footnote_def(b, ctx, x, y, max_x);
    InlineRender ir = ir_make(ctx, y, x, max_x, ctx->_t.cw, ctx->_t.lh);
    ir_emit_inlines(&ir, b->inlines, b->inline_count);
    return ir_finish(&ir) + ctx->_t.unit * 0.78f;
}

/* Definition-list term — rendered bright/bold at the left margin. */
static f32 render_def_term(const MdBlock *b, MdRenderCtx *ctx,
                           f32 x, f32 y, f32 max_x) {
    InlineRender ir = ir_make(ctx, y, x, max_x, ctx->_t.cw, ctx->_t.lh);
    ir.fg = ctx->_fg_bright;
    ir_emit_runs(&ir, b->inlines, b->inline_count, ctx->_fg_bright, false, false);
    return ir_finish(&ir);
}

/* Definition-list description — indented under its term. */
static f32 render_def_desc(const MdBlock *b, MdRenderCtx *ctx,
                           f32 x, f32 y, f32 max_x) {
    const MdTypo *t = &ctx->_t;
    InlineRender ir = ir_make(ctx, y, x + t->indent, max_x, t->cw, t->lh);
    ir.fg = ctx->_muted;
    ir_emit_inlines(&ir, b->inlines, b->inline_count);
    return ir_finish(&ir) + t->unit * 0.30f;
}

/* Headings carry their own leading space, so the gap above one collapses
 * naturally onto the previous block's trailing space instead of stacking into
 * a hole. No underline rule: at this size the step in weight and the space
 * around it already say "new section". */
static f32 render_heading(const MdBlock *b, MdRenderCtx *ctx,
                           f32 x, f32 y, f32 max_x) {
    const MdTypo *t = &ctx->_t;
    u8 lvl = b->heading_level <= 6 ? b->heading_level : 6;
    if (lvl == 0) lvl = 1;
    const MdHeadStep *st = &H_STEP[lvl];
    f32 hcw, hlh;
    md_head_metrics(ctx, lvl, &hcw, &hlh);

    /* An H1 that opens the document should not push a screenful of air above
     * itself — the viewport padding already separates it from the title bar. */
    f32 top = t->unit * st->top;
    if (y <= ctx->y - ctx->scroll_y + 0.5f) top = t->unit * 0.15f;
    y += top;

    if (!ctx->measure_only) renderer_set_ui_scale(ctx->r, hcw, hlh);
    InlineRender ir = ir_make(ctx, y, x, max_x, hcw, hlh);
    ir.fg        = lvl <= 2 ? ctx->_heading_bright : ctx->_heading_fg;
    ir.fg_bright = ctx->_heading_bright;
    ir_emit_inlines(&ir, b->inlines, b->inline_count);
    f32 end_y = ir_finish(&ir);
    /* Restore the body scale — renderer_reset_ui_scale would drop back to the
     * atlas's natural metrics instead of the size this document is set in,
     * leaving every following block with mismatched glyph height vs. line
     * advance and overlapping text. */
    if (!ctx->measure_only) renderer_set_ui_scale(ctx->r, t->cw, t->lh);
    return end_y + t->unit * st->bottom;
}

/* Tab stop for fenced code body. CommonMark says "tabs are preserved" — for
 * display they expand to the next multiple of TAB_WIDTH so indented code
 * reads naturally instead of collapsing to single cells. */
#define MD_CODE_TAB_WIDTH 4u

/* =========================================================================
 * Code-block syntax highlighting
 *
 * Fenced blocks are coloured by ui/syntax.c — the same scanner the code editor
 * and the diff dock use — so a ```go snippet in a note reads exactly like the
 * .go file it was pasted from. The fence info string resolves through
 * syntax_lang_for_tag(), which knows every language the editor does.
 * ========================================================================= */

/* Theme colour for a scanned token, from the palette md_render already
 * derived against the code panel's background (so contrast is checked once,
 * not per glyph). */
static Color code_span_color(const MdRenderCtx *ctx, SynKind k, Color deflt) {
    switch (k) {
    case SYN_KEYWORD: return ctx->_code_kw;
    case SYN_STRING:  return ctx->_code_str;
    case SYN_COMMENT: return ctx->_code_cmt;
    case SYN_NUMBER:  return ctx->_code_num;
    case SYN_PREPROC: return ctx->_code_pre;
    case SYN_TYPE:    return ctx->_code_typ;
    case SYN_TAG:     return ctx->_code_typ;
    case SYN_FUNC:    return ctx->_code_fn;
    case SYN_ATTR:    return ctx->_code_fn;
    case SYN_INSERT:  return ctx->_code_ins;
    case SYN_DELETE:  return ctx->_code_del;
    default:          return deflt;
    }
}

static f32 render_math_block(const MdBlock *b, MdRenderCtx *ctx,
                             f32 x, f32 y, f32 max_x);
static f32 render_diagram_block(const MdBlock *b, MdRenderCtx *ctx,
                                f32 x, f32 y, f32 max_x);

/* Accent hue for a fenced block, keyed to its language.
 *
 * The bar down the left of a code block identifies the block while scrolling
 * past it, and because it indexes the THEME's ANSI ramp rather than naming an
 * RGB value it re-skins with the theme like everything else in the reader.
 *
 * A fence with no language — or one this build's scanner does not know — gets a
 * neutral bar rather than an arbitrary hue, so a hue always names a language.
 */
static Color md_lang_accent(const MdRenderCtx *ctx, SynLang lang) {
    const Theme *t = ctx->theme;
    i32 slot;
    switch (lang) {
    case SYN_LANG_SHELL:  case SYN_LANG_SCRIPT:
    case SYN_LANG_DOCKER: case SYN_LANG_POWERSHELL:
    case SYN_LANG_BATCH:  case SYN_LANG_DIFF:   slot = 10; break;  /* green   */
    case SYN_LANG_PYTHON: case SYN_LANG_C:
    case SYN_LANG_LUA:    case SYN_LANG_R:
    case SYN_LANG_JULIA:  case SYN_LANG_NIM:
    case SYN_LANG_HASKELL: case SYN_LANG_OCAML:
    case SYN_LANG_LISP:   case SYN_LANG_ERLANG: slot = 12; break;  /* blue    */
    case SYN_LANG_JS:     case SYN_LANG_JSON:
    case SYN_LANG_HTML:   case SYN_LANG_MARKUP:
    case SYN_LANG_VUE:    case SYN_LANG_SVELTE:
    case SYN_LANG_ASTRO:  case SYN_LANG_TEMPLATE:
    case SYN_LANG_JINJA:  case SYN_LANG_ERB:
    case SYN_LANG_EJS:    case SYN_LANG_HEEX:
    case SYN_LANG_JSP:    case SYN_LANG_JSONNET: slot = 11; break; /* yellow  */
    case SYN_LANG_RUST:   case SYN_LANG_RUBY:
    case SYN_LANG_SWIFT:  case SYN_LANG_JVM:
    case SYN_LANG_ZIG:    case SYN_LANG_ELIXIR:
    case SYN_LANG_SOLIDITY:                     slot =  9; break;  /* red     */
    case SYN_LANG_GO:     case SYN_LANG_SQL:
    case SYN_LANG_YAML:   case SYN_LANG_TOML:
    case SYN_LANG_GOMOD:  case SYN_LANG_HCL:
    case SYN_LANG_NIX:    case SYN_LANG_GRAPHQL:
    case SYN_LANG_PROTO:  case SYN_LANG_PRISMA: slot = 14; break;  /* cyan    */
    case SYN_LANG_PHP:    case SYN_LANG_CSS:
    case SYN_LANG_MAKE:   case SYN_LANG_ASM:
    case SYN_LANG_LATEX:  case SYN_LANG_WASM:   slot = 13; break;  /* magenta */
    default:                                    slot = -1; break;
    }
    if (slot < 0) return color_with_alpha(t->fg, 0.30f);
    /* Against the PANEL, not against the page: the bar and the label both sit
     * on the code surface, and a theme whose bright-green is close to its panel
     * colour would otherwise produce a marker nobody can see. */
    return color_ensure_contrast(t->ansi[slot], ctx->_code_block_panel_bg, 2.6f);
}

/* The header label is the fence's FIRST info word, upper-cased.
 *
 * Two things it is not. It is not `b->info` verbatim: an info string carries
 * attributes ("```js title=main.js"). And it is not the language's canonical
 * display name from `syntax_lang_name()`: the scanner's buckets are coarse on
 * purpose (one JS lexer serves .ts and .tsx), so a ```ts fence would be
 * labelled "JavaScript". The author's own word is the label.
 *
 * The caps are doing real work: at the code size, in the code font, lower-case
 * "bash" reads as the first line of the block. Caps plus a smaller size are
 * what separate chrome from content without adding another colour. */
static u32 md_code_label(MdSlice info, char *out, u32 cap) {
    u32 n = 0;
    for (u32 i = 0; i < info.len && n + 1 < cap; i++) {
        u8 c = info.data[i];
        if (c == ' ' || c == '\t' || c == '{' || c == ',' || c == ':') break;
        if (c >= 'a' && c <= 'z') c = (u8)(c - 32);
        out[n++] = (char)c;
    }
    out[n] = '\0';
    return n;
}

/* Source rendering is also the terminal fallback for failed native layouts.
 * It must never redispatch the unchanged math/diagram-tagged block. */
static f32 render_code_source(const MdBlock *b, MdRenderCtx *ctx,
                               f32 x, f32 y, f32 max_x);

static f32 render_code(const MdBlock *b, MdRenderCtx *ctx,
                        f32 x, f32 y, f32 max_x) {
    /* A ```math / ```latex / ```tex fence renders as display math. */
    if (b->info.len && (slice_eq_ci_cstr(b->info, "math") ||
                        slice_eq_ci_cstr(b->info, "latex") ||
                        slice_eq_ci_cstr(b->info, "tex")))
        return render_math_block(b, ctx, x, y, max_x);

    /* A ```mermaid fence with a supported diagram type renders natively;
     * anything that cannot be laid out falls through to the code path
     * below. */
    if (b->info.len && slice_eq_ci_cstr(b->info, "mermaid") &&
        md_diagram_probe(b->raw.data, b->raw.len) != MD_DIAGRAM_NONE)
        return render_diagram_block(b, ctx, x, y, max_x);

    return render_code_source(b, ctx, x, y, max_x);
}

static f32 render_code_source(const MdBlock *b, MdRenderCtx *ctx,
                        f32 x, f32 y, f32 max_x) {
    const MdTypo *t = &ctx->_t;
    f32 cw = t->cw;
    /* Code gets tighter leading than prose: it is scanned in columns, and
     * body leading pulls the lines of a block apart until it stops reading as
     * one unit. */
    f32 ch = roundf(t->gh * MD_CODE_LEADING);

    f32 outer_pad_x = t->pad_x;
    f32 inner_pad_y = t->pad_y;
    /* Always reserve a header band: it carries the language label (when set)
     * and the Copy button, so every fenced block gets a consistent chrome. */
    f32 header_h    = roundf(t->lh * 1.22f);

    /* Reserve the last column for the wrap-continuation arrow so it stays
     * inside the panel instead of bleeding past max_x. */
    u32 max_cells = md_cells_fit(max_x - x, cw);
    if (max_cells < 8) max_cells = 8;
    if (max_cells > 1) max_cells--;

    /* Always count WRAPPED visual lines, on-screen or not.
     *
     * A soft-wrapped code block is taller than its newline count, so counting
     * newlines off-screen would give a document measured with the block
     * scrolled away a different total height than the same document measured
     * with it in view. That total is the scroll extent, and the view would
     * shift under the reader by a code line while scrolling past. The walk runs
     * ONCE per block per layout generation (md_render caches block heights and
     * skips render_block entirely on a cache hit), so the full count costs next
     * to nothing. */
    u32 lines = 0;
    {
        u32 i = 0;
        while (i < b->raw.len) {
            u32 ls = i;
            while (i < b->raw.len && b->raw.data[i] != '\n') i++;
            u32 le = i;
            if (le > ls && b->raw.data[le - 1] == '\r') le--;

            u32 cells = 0;
            u32 vlines = 1;
            u32 j = ls;
            while (j < le) {
                u32 cp = 0;
                u32 n = utf8_decode(b->raw.data + j, le - j, &cp);
                if (n == 0) { j++; continue; }
                j += n;

                u32 step;
                if (cp == '\t')   step = MD_CODE_TAB_WIDTH - (cells % MD_CODE_TAB_WIDTH);
                else if (cp < 32) step = 0;
                else              step = 1;

                while (step > 0) {
                    if (cells >= max_cells) { vlines++; cells = 0; }
                    u32 avail = max_cells - cells;
                    u32 take  = step < avail ? step : avail;
                    cells += take;
                    step  -= take;
                }
            }
            lines += vlines;
            if (i < b->raw.len) i++;
        }
        if (lines == 0) lines = 1;
    }

    f32 body_h  = (f32)lines * ch + 2.0f * inner_pad_y;
    f32 panel_h = header_h + body_h;

    /* Off-screen: every draw call below is a no-op anyway, but the body
     * loop still spends real CPU decoding UTF-8 and computing wrap
     * positions. Short-circuit so scrolled-away code blocks cost almost
     * nothing. */
    if (ctx->measure_only) return y + panel_h + t->unit * 0.72f;

    /* Resolved once, above the header: the language drives the accent hue and
     * the label as well as the body's highlighting. */
    SynLang lang = (SynLang)syntax_lang_for_tag((const char *)b->info.data,
                                                (usize)b->info.len);

    Color panel_bg     = ctx->_code_block_panel_bg;
    Color accent       = md_lang_accent(ctx, lang);
    Color code_text_fg = ctx->_code_block_text_fg;

    /* Draw the whole panel — header chrome and body — on the code line box, so
     * every glyph in it is centred in the same box the layout advances by.
     * Restored just before returning. */
    renderer_set_ui_scale(ctx->r, cw, ch);

    f32 cb_r  = t->radius;
    f32 pnl_x = x - outer_pad_x;
    f32 pnl_w = max_x - x + 2 * outer_pad_x;
    f32 bar_w = fmaxf(2.0f, roundf(ctx->dpi * 1.5f));
    cr_draw_rrect(ctx, pnl_x, y, pnl_w, panel_h, panel_bg, cb_r);

    /* Header band.
     *
     * The tint steps the PANEL's own colour toward black/white by a fixed
     * amount, which is visible on every theme by construction, light and dark
     * alike, and cannot clash with the palette because it is made of the
     * panel. A palette colour such as `ansi[8]` can sit so near the panel's
     * that header and body read as one slab.
     *
     * Full width with both top corners rounded, drawn BEFORE the accent bar so
     * the bar overlays it and no notch of panel colour is left in the top-left
     * corner. */
    Color hdr_bg = color_lerp(panel_bg,
                              ctx->_is_light ? (Color){0,0,0,1} : (Color){1,1,1,1},
                              ctx->_is_light ? 0.050f : 0.065f);
    hdr_bg.a = panel_bg.a;
    f32 hair = fmaxf(1.0f, roundf(t->hair));
    renderer_draw_rrect(ctx->r, pnl_x, y, pnl_w, header_h, hdr_bg,
                        cb_r, cb_r, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    renderer_draw_rrect_simple(ctx->r, pnl_x, y + header_h - hair, pnl_w, hair,
                               color_with_alpha(ctx->_rule, 0.85f), 0.0f);

    /* Left accent bar — inset by the corner radius and capsule-ended, so it
     * reads as a marker sitting inside the panel rather than as a clipped edge.
     * Drawn last of the three so it crosses the header divider in one piece. */
    cr_draw_rrect(ctx, pnl_x, y + cb_r, bar_w, panel_h - 2 * cb_r,
                  accent, bar_w * 0.5f);

    /* ---- header contents -------------------------------------------------
     *
     * Chrome, not content, and set as such: one size down from the code, in
     * caps, on its own line box. Both ends are inset from the panel by the same
     * amount, so the label and the Copy chip line up across the band.
     *
     * Neither the label nor the caption is recorded in the glyph collector:
     * they are furniture, and a drag across the document selects only the code.
     */
    if (header_h > 0) {
        f32 label_cw = fmaxf(1.0f, roundf(cw * 0.86f));
        f32 label_gh = md_glyph_h(ctx->r, label_cw);
        f32 hdr_in   = roundf(outer_pad_x * 0.85f);
        renderer_set_ui_scale(ctx->r, label_cw, header_h);

        /* Language label — left, in the block's own accent. */
        char lbuf[24];
        u32  llen = md_code_label(b->info, lbuf, sizeof lbuf);
        if (llen) {
            InlineRender ir = ir_make(ctx, y, pnl_x + bar_w + hdr_in,
                                      max_x, label_cw, header_h);
            ir.glyph_buf = NULL;
            ir.fg = color_with_alpha(accent, 0.95f);
            MdSlice ls = { (const u8 *)lbuf, llen };
            ir_emit_bytes(&ir, ls, ir.fg, false, false);
        }

        /* Copy button — right. A ghost chip: nothing but the icon and its
         * caption at rest, a filled pill under the pointer, and a green check
         * for a beat after the click, so a click visibly lands.
         *
         * Sized for the LONGER caption in both states and centred inside it, so
         * confirming does not shuffle the header sideways. */
        bool flashed = (ctx->copy_flash > 0.0f &&
                        ctx->copy_flash_block == b->raw.data);
        const char *cap  = flashed ? "Copied" : "Copy";
        f32 cap_cells    = 6.0f;                       /* strlen("Copied") */
        f32 icon_sz      = roundf(label_gh * 1.15f);
        f32 gap          = roundf(label_cw * 0.55f);
        f32 chip_pad     = roundf(label_cw * 0.85f);
        f32 chip_h       = roundf(header_h * 0.72f);
        f32 chip_w       = icon_sz + gap + cap_cells * label_cw + 2.0f * chip_pad;
        f32 chip_x       = pnl_x + pnl_w - hdr_in - chip_w;
        f32 chip_y       = y + roundf((header_h - chip_h) * 0.5f);
        bool hot = ctx->hover_x >= chip_x && ctx->hover_x < chip_x + chip_w &&
                   ctx->hover_y >= chip_y && ctx->hover_y < chip_y + chip_h;

        Color ok_c   = color_ensure_contrast(ctx->theme->ansi[10], hdr_bg, 3.0f);
        f32   lift   = flashed ? ctx->copy_flash : (hot ? 1.0f : 0.0f);
        Color chip_c = flashed ? color_with_alpha(ok_c, 0.16f * ctx->copy_flash)
                               : color_with_alpha(ctx->_fg, 0.10f * lift);
        Color ink    = flashed
                     ? color_with_alpha(ok_c, 0.55f + 0.45f * ctx->copy_flash)
                     : color_with_alpha(ctx->_fg, hot ? 0.95f : 0.55f);
        if (chip_c.a > 0.001f)
            cr_draw_rrect(ctx, chip_x, chip_y, chip_w, chip_h, chip_c,
                          roundf(chip_h * 0.34f));

        f32 content_w = icon_sz + gap + (f32)strlen(cap) * label_cw;
        f32 cx = chip_x + roundf((chip_w - content_w) * 0.5f);
        icon_draw(ctx->r, flashed ? ICON_CHECK : ICON_COPY,
                  cx, chip_y + roundf((chip_h - icon_sz) * 0.5f), icon_sz, ink);
        {
            /* Laid out on the BAND's line box, not the chip's, because the
             * renderer centres each glyph in whatever `ch` the ui scale was
             * last set to — one scale for the whole header. Handing ir_make a
             * chip-height box while the scale still says header-height would
             * put the caption half the difference below its own pill. The chip
             * is centred in the band, so the band's centre IS the chip's. */
            InlineRender ir = ir_make(ctx, y, cx + icon_sz + gap,
                                      pnl_x + pnl_w, label_cw, header_h);
            ir.glyph_buf = NULL;
            ir.fg = ink;
            MdSlice cs = { (const u8 *)cap, (u32)strlen(cap) };
            ir_emit_bytes(&ir, cs, ir.fg, false, false);
        }

        /* Click target — `url` is the code text, consumed by fb_md_hit_copy. */
        if (ctx->link_rects && ctx->link_rect_count &&
            *ctx->link_rect_count < ctx->link_rect_cap) {
            ctx->link_rects[*ctx->link_rect_count] = (MdLinkRect){
                .x = chip_x, .y = chip_y, .w = chip_w, .h = chip_h,
                .url = b->raw.data, .url_len = b->raw.len,
                .kind = MD_LINKRECT_COPY };
            (*ctx->link_rect_count)++;
        }
        renderer_set_ui_scale(ctx->r, cw, ch);   /* back to the code metrics */
    }

    f32 line_y = y + header_h + inner_pad_y;
    Color cont_fg = color_with_alpha(code_text_fg, 0.45f);
    bool highlight = (lang != SYN_LANG_NONE);
    u32  syn_state = 0;          /* block comments / templates span lines */

    u32 i = 0;
    while (i < b->raw.len) {
        u32 line_start = i;
        while (i < b->raw.len && b->raw.data[i] != '\n') i++;
        u32 line_end = i;
        if (line_end > line_start && b->raw.data[line_end - 1] == '\r') line_end--;

        /* Same placeholder-trap as the inline path: pre-cache non-ASCII
         * glyphs on this line so the first frame already has real UVs. */
        font_warm_text_glyphs_n(&ctx->r->font,
                                (const char *)(b->raw.data + line_start),
                                (usize)(line_end - line_start));

        /* Whole line at once: the scanner's state word must see every byte,
         * including the ones a soft wrap pushes onto the next visual row. */
        SynSpan spans[SYN_MAX_SPANS];
        i32 nspans = 0;
        if (highlight)
            nspans = syntax_scan(lang, (const char *)(b->raw.data + line_start),
                                 line_end - line_start, &syn_state,
                                 spans, SYN_MAX_SPANS);
        i32 si = 0;

        f32 cx = x;
        u32 cells_emitted = 0;
        /* What the selection list is told about the next glyph: after a wrap,
         * that its row continues the line above. */
        u8  gflags = 0;
        /* An empty line draws nothing, so nothing would stand for it in the
         * list and a copy closed the gap up. A widthless marker keeps the
         * line. */
        if (line_end == line_start)
            cr_note_glyph(ctx, x, line_y, 0.0f, 0, 0);
        u32 j = line_start;
        while (j < line_end) {
            /* Pick the next chunk + colour: either the span covering this
             * byte, or the default run up to where the next span starts. With
             * no language the walk goes one codepoint at a time so the
             * tab/wrap pacing stays intact; only the colour differs. */
            u32 chunk_end;
            Color tok_color = code_text_fg;
            if (highlight) {
                u32 rel = j - line_start;
                while (si < nspans && spans[si].off + spans[si].len <= rel) si++;
                if (si < nspans && rel >= spans[si].off) {
                    chunk_end = line_start + spans[si].off + spans[si].len;
                    tok_color = code_span_color(ctx, (SynKind)spans[si].kind,
                                                code_text_fg);
                } else {
                    chunk_end = (si < nspans) ? line_start + spans[si].off
                                              : line_end;
                }
                if (chunk_end <= j) chunk_end = j + 1;
                if (chunk_end > line_end) chunk_end = line_end;
            } else {
                u32 cp_probe = 0;
                u32 n_probe  = utf8_decode(b->raw.data + j, line_end - j, &cp_probe);
                chunk_end = j + (n_probe ? n_probe : 1);
            }

            u32 t = j;
            while (t < chunk_end) {
                u32 cp = 0;
                i32 cols = 1;
                /* Grapheme-aware: a fenced block is still a character grid, and
                 * a ZWJ family or a flag pair is ONE glyph two columns wide.
                 * Per-codepoint decoding drew "🇹🇷" as two lettered boxes and
                 * let every emoji overlap the character after it. */
                u32 n = utf8_next_grapheme(b->raw.data + t, chunk_end - t, &cp, &cols);
                if (n == 0) { t++; continue; }
                t += n;

                if (cp == '\t') {
                    u32 step = MD_CODE_TAB_WIDTH - (cells_emitted % MD_CODE_TAB_WIDTH);
                    /* A tab is a gap on screen and a character in the text:
                     * recorded once, as wide as the part of it on this row,
                     * so a copied Makefile still has its tabs. */
                    bool noted = false;
                    while (step > 0) {
                        if (cells_emitted >= max_cells) {
                            cr_push_glyph_f(ctx, cx, line_y, 0x21AA, cont_fg, MD_GLYPH_CHROME);
                            line_y += ch;
                            cx = x;
                            cells_emitted = 0;
                            gflags = MD_GLYPH_WRAP;
                        }
                        u32 avail = max_cells - cells_emitted;
                        u32 take  = step < avail ? step : avail;
                        if (!noted) {
                            cr_note_glyph(ctx, cx, line_y, (f32)take * cw, '\t', gflags);
                            gflags = 0;
                            noted = true;
                        }
                        cx            += (f32)take * cw;
                        cells_emitted += take;
                        step          -= take;
                    }
                    continue;
                }
                if (cp < 32) continue;
                if (cols <= 0) cols = 1;

                if (cells_emitted + (u32)cols > max_cells) {
                    /* The arrow is chrome: it says "this row goes on", which
                     * is true of the screen and not of the code. */
                    cr_push_glyph_f(ctx, cx, line_y, 0x21AA, cont_fg, MD_GLYPH_CHROME);
                    line_y += ch;
                    cx = x;
                    cells_emitted = 0;
                    gflags = MD_GLYPH_WRAP;
                }
                cr_push_glyph_f(ctx, cx, line_y, cp, tok_color, gflags);
                gflags = 0;
                cx += cw * (f32)cols;
                cells_emitted += (u32)cols;
            }
            j = chunk_end;
        }
        line_y += ch;
        if (i < b->raw.len) i++;
    }
    renderer_set_ui_scale(ctx->r, t->cw, t->lh);   /* back to the body scale */
    return y + panel_h + t->unit * 0.72f;
}

/* Use the conventional filled → hollow → square bullet sequence to indicate
 * nesting depth without relying on indentation alone. */
static u32 list_bullet_cp(u8 depth) {
    switch (depth % 3) {
    case 0:  return 0x2022;   /* • */
    case 1:  return 0x25E6;   /* ◦ */
    default: return 0x25AA;   /* ▪ */
    }
}

/* Common geometry for the two list-ish blocks: where the marker column starts
 * and where its text hangs. Text is aligned on a single hanging indent so a
 * wrapped second line lines up under the first character, not under the
 * bullet. */
static void list_geometry(const MdRenderCtx *ctx, const MdBlock *b,
                          f32 x, f32 *marker_x, f32 *text_x) {
    const MdTypo *t = &ctx->_t;
    f32 depth = (f32)(b->list_depth > 8 ? 8 : b->list_depth);
    *marker_x = x + depth * t->indent;
    /* Ordered lists get a wider gutter so the number can be right-aligned
     * inside it (fits "99." plus a space). A per-item gutter would make the
     * text column jump between "9." and "10."; a fixed one keeps every label
     * in the list on the same edge. */
    *text_x   = *marker_x + roundf(t->cw * (b->list_ordered ? 3.0f : 2.0f));
}

/* An ordered item's number, right-aligned inside the gutter one cell clear of
 * the text, so "9." and "10." end on the same edge. A number too wide for the
 * gutter (100+) starts at the margin instead of hanging off it. */
static void list_draw_ordinal(const MdRenderCtx *ctx, const MdBlock *b,
                              f32 marker_x, f32 text_x, f32 y) {
    const MdTypo *t = &ctx->_t;
    char num[12];
    i32 n = snprintf(num, sizeof num, "%u.",
                     (unsigned)(b->list_index ? b->list_index : 1));
    f32 nx = text_x - t->cw - (f32)n * t->cw;
    if (nx < marker_x) nx = marker_x;
    for (i32 k = 0; k < n; k++) {
        cr_push_glyph(ctx, nx, y, (u32)(u8)num[k], ctx->_marker);
        nx += t->cw;
    }
}

static f32 render_list_item(const MdBlock *b, MdRenderCtx *ctx,
                             f32 x, f32 y, f32 max_x) {
    const MdTypo *t = &ctx->_t;
    f32 marker_x, text_x;
    list_geometry(ctx, b, x, &marker_x, &text_x);

    if (b->list_ordered) {
        list_draw_ordinal(ctx, b, marker_x, text_x, y);
    } else {
        cr_push_glyph(ctx, marker_x, y, list_bullet_cp(b->list_depth),
                      ctx->_marker);
    }
    InlineRender ir = ir_make(ctx, y, text_x, max_x, t->cw, t->lh);
    ir.glyph_flags = MD_GLYPH_GAP;     /* "• text", not "•text", in a copy */
    ir_emit_inlines(&ir, b->inlines, b->inline_count);
    return ir_finish(&ir) + t->unit * 0.12f;
}

static f32 render_task_item(const MdBlock *b, MdRenderCtx *ctx,
                             f32 x, f32 y, f32 max_x) {
    const MdTypo *t = &ctx->_t;
    f32 box_x, text_x;
    list_geometry(ctx, b, x, &box_x, &text_x);
    /* "1. [ ] task": the number keeps its gutter and the box takes the place
     * the text would have had, with the text one box further in. */
    if (b->list_ordered) {
        list_draw_ordinal(ctx, b, box_x, text_x, y);
        box_x  = text_x;
        text_x = box_x + roundf(t->cw * 2.0f);
    }

    u8 st = b->task_state;
    bool done      = (st == 'x' || st == 'X');
    bool cancelled = (st == '-');

    /* The box is sized off the type, not off the line box, so it keeps its
     * proportion to the text when the leading changes. Centred on the ink
     * (baseline − x-height/2) rather than on the line box, so the checkbox
     * does not float above its label. */
    f32 box_sz = roundf(t->cw * 1.30f);
    if (box_sz < 8.0f) box_sz = 8.0f;
    f32 box_y  = y + md_baseline(ctx->r, t->lh, t->gh)
                   - roundf(t->gh * 0.32f) - box_sz * 0.5f;
    f32 box_r  = roundf(box_sz * 0.28f);

    Color accent = done      ? ctx->theme->ansi[10]
                 : cancelled ? color_with_alpha(ctx->_fg, 0.35f)
                             : ctx->theme->ansi[11];
    if (st == 0) {
        /* Empty: a hairline ring. Drawn as a filled rrect with the panel colour
         * punched back out, because the renderer has no stroked-rrect
         * primitive. */
        cr_draw_rrect(ctx, box_x, box_y, box_sz, box_sz,
                      color_with_alpha(ctx->_fg, 0.42f), box_r);
        f32 in = fmaxf(1.0f, roundf(ctx->dpi));
        cr_draw_rrect(ctx, box_x + in, box_y + in,
                      box_sz - 2 * in, box_sz - 2 * in,
                      ctx->theme->bg, fmaxf(0.0f, box_r - in));
    } else {
        cr_draw_rrect(ctx, box_x, box_y, box_sz, box_sz,
                      done || cancelled ? accent
                                        : color_with_alpha(accent, 0.85f),
                      box_r);
        /* ✓ for done, ✕ for cancelled, else the literal marker (/, >, …),
         * centred in the box: the glyph is drawn centred inside a cell of
         * (cw × line box), so aligning centres — not corners — is what puts
         * the mark in the middle. */
        u32 mark = done ? 0x2713 : (cancelled ? 0x2715 : (u32)st);
        cr_push_chrome(ctx, box_x + (box_sz - t->cw) * 0.5f,
                       box_y + (box_sz - t->lh) * 0.5f,
                       mark, ctx->theme->bg);
    }
    /* The box is drawn, not typed, and an empty one is two rectangles — so in
     * the selection list a task stands as what it is in the source, "[x]" or "[
     * ]", on the item's own line. A copied checklist then says which items are
     * done. */
    cr_note_glyph(ctx, box_x, y, box_sz, '[', b->list_ordered ? MD_GLYPH_GAP : 0);
    cr_note_glyph(ctx, box_x + box_sz, y, 0.0f, st ? (u32)st : (u32)' ', 0);
    cr_note_glyph(ctx, box_x + box_sz, y, 0.0f, ']', 0);

    /* Emit the clickable checkbox rect (real draw pass only — measure passes
     * must not record duplicate/extra rects). Padded to a comfortable target:
     * the drawn box is deliberately small, the hit area should not be.
     *
     * Two cases must NOT produce a rect, because a click on one would write to
     * the wrong bytes: a task whose source offset the parser could not vouch
     * for (MD_TASK_NO_SRC — callout/quote bodies), and any task inside a
     * ![[transclusion]], whose offsets index the EMBEDDED file while the click
     * handler edits the host. Both render normally; they just aren't editable
     * from here. */
    if (!ctx->measure_only && ctx->task_rects && ctx->task_rect_count &&
        b->src_off != MD_TASK_NO_SRC && ctx->embed_depth == 0 &&
        *ctx->task_rect_count < ctx->task_rect_cap) {
        f32 grow = roundf(t->cw * 0.4f);
        ctx->task_rects[*ctx->task_rect_count] = (MdTaskRect){
            .x = box_x - grow, .y = box_y - grow,
            .w = box_sz + 2 * grow, .h = box_sz + 2 * grow,
            .src_off = b->src_off,
        };
        (*ctx->task_rect_count)++;
    }

    InlineRender ir = ir_make(ctx, y, text_x, max_x, t->cw, t->lh);
    ir.glyph_flags = MD_GLYPH_GAP;
    /* A completed task reads as faded AND struck through — the two together
     * are what make a checked list scannable. The strike rides the inline
     * emitter rather than being one rect afterwards, so a wrapped item gets it
     * on every visual line. */
    if (done || cancelled) {
        ir.fg = ctx->_muted;
        ir_emit_runs(&ir, b->inlines, b->inline_count, ctx->_muted, false, true);
    } else {
        ir_emit_inlines(&ir, b->inlines, b->inline_count);
    }
    return ir_finish(&ir) + t->unit * 0.12f;
}

/* ---- block boundaries in the selection list ----
 * Two blocks are apart on screen by some pixels; in the text they are apart by
 * a blank line, and the glyph rects cannot tell the first from a line break.
 * Whoever walks a list of blocks flags the first glyph each one recorded. */

static u32 glyph_list_len(const MdRenderCtx *ctx) {
    return (ctx->glyph_rects && ctx->glyph_rect_count) ? *ctx->glyph_rect_count : 0u;
}

/* Does the text set `cur` apart from `prev` with a blank line? Everything is,
 * except the lines that belong together: the items of a list (a loose list
 * copies tight — the blank lines between its items are spacing, not content)
 * and a definition under its term. */
static bool block_text_gap(const MdBlock *prev, const MdBlock *cur) {
    if (!prev) return false;
    bool prev_item = prev->kind == MD_BLOCK_LIST_ITEM || prev->kind == MD_BLOCK_TASK_ITEM;
    bool cur_item  = cur->kind == MD_BLOCK_LIST_ITEM || cur->kind == MD_BLOCK_TASK_ITEM;
    if (prev_item && cur_item) return false;
    if (cur->kind == MD_BLOCK_DEF_DESC &&
        (prev->kind == MD_BLOCK_DEF_TERM || prev->kind == MD_BLOCK_DEF_DESC)) return false;
    return true;
}

/* `cur` has just been rendered and recorded its glyphs from `first_glyph` on. */
static void block_mark_text(const MdRenderCtx *ctx, u32 first_glyph,
                            const MdBlock *prev, const MdBlock *cur) {
    if (first_glyph >= glyph_list_len(ctx)) return;       /* it recorded none */
    if (block_text_gap(prev, cur))
        ctx->glyph_rects[first_glyph].flags |= MD_GLYPH_BLOCK;
}

/* Forward decl: callout/quote bodies recurse through the block dispatcher to
 * render their parsed child blocks (lists, paragraphs, nested callouts). */
static f32 render_block(const MdBlock *b, MdRenderCtx *ctx,
                        f32 x, f32 y, f32 max_x);

/* Render a block's child blocks stacked vertically from `y`, returning the y
 * just below the last child. */
static f32 render_children(const MdBlock *b, MdRenderCtx *ctx,
                           f32 x, f32 y, f32 max_x) {
    if (!b || !b->children || b->child_count == 0) return y;
    bool save_nested = ctx->in_nested_block;
    ctx->in_nested_block = true;
    f32 cy = y;
    for (u32 i = 0; i < b->child_count; i++) {
        /* cached_h / cached_gen are the documented cross-frame layout cache
         * (md_ast.h) — mutable state on an otherwise read-only AST, which is
         * why this casts away the const it inherited from render_block. */
        MdBlock *ch = (MdBlock *)&b->children[i];
        bool save_measure = ctx->measure_only;
        /* Cull children outside the viewport down to a measure. The top-level
         * loop does this for top-level blocks; a container does it for its own
         * subtree, so one long callout or transclusion does not emit glyphs
         * for content nobody can see, or exhaust the glyph-rect collector
         * before the on-screen blocks get to it.
         *
         * Only cull on a height actually measured under this layout: with a
         * guessed height, a child taller than the viewport would be culled
         * while still on screen. The container's own measure pass fills the
         * cache, so by the draw pass the heights are there. */
        /* Never on the copy frame: collect_all means every block is drawn so
         * that the glyph list is the whole document, and a culled child left
         * a hole in the copy exactly where its container ran off screen. */
        if (!save_measure && !ctx->collect_all &&
            ch->cached_gen == ctx->_layout_gen && ch->cached_h > 0.0f) {
            if (cy + ch->cached_h < ctx->y || cy > ctx->y + ctx->h)
                ctx->measure_only = true;
        }
        u32 first_glyph = glyph_list_len(ctx);
        f32 next = render_block(ch, ctx, x, cy, max_x);
        block_mark_text(ctx, first_glyph, i ? &b->children[i - 1] : NULL, ch);
        f32 h = next - cy;
        if (isfinite(h) && h >= 0.0f) {
            ch->cached_h   = h;
            ch->cached_gen = ctx->_layout_gen;
        }
        ctx->measure_only = save_measure;
        cy = next;
    }
    ctx->in_nested_block = save_nested;
    return cy;
}

/* Blockquote: a tinted card with a rounded accent rail.
 *
 * Content is laid out twice — once measured, to size the card, then once for
 * real on top of it. The measure pass MUST go through ctx->measure_only,
 * because that is what suppresses the link/glyph/task collectors; laying out
 * twice in draw mode would record every rect (and every selectable glyph)
 * twice. Same contract as render_callout below. */
static f32 quote_layout(const MdBlock *b, MdRenderCtx *ctx,
                        f32 inner_x, f32 body_y, f32 max_x) {
    const MdTypo *t = &ctx->_t;
    if (b->child_count) {
        f32 e = render_children(b, ctx, inner_x, body_y, max_x);
        e -= t->unit * 0.5f;               /* drop the last child's trailing gap */
        return e < body_y ? body_y + t->lh : e;
    }
    InlineRender ir = ir_make(ctx, body_y, inner_x, max_x, t->cw, t->lh);
    ir.fg = ctx->_muted;
    ir_emit_inlines(&ir, b->inlines, b->inline_count);
    return ir_finish(&ir);
}

static f32 render_quote(const MdBlock *b, MdRenderCtx *ctx,
                         f32 x, f32 y, f32 max_x) {
    const MdTypo *t = &ctx->_t;
    f32 bar_w   = fmaxf(2.0f, roundf(ctx->dpi * 1.5f));
    f32 inner_x = x + bar_w + t->pad_x * 1.4f;
    f32 body_y  = y + t->pad_y;
    f32 inner_max_x = max_x - t->pad_x;

    bool save_measure = ctx->measure_only;
    bool save_exact   = ctx->measure_exact;
    ctx->measure_only  = true;
    ctx->measure_exact = true;
    f32 end_y = quote_layout(b, ctx, inner_x, body_y, inner_max_x) + t->pad_y;
    ctx->measure_only  = save_measure;
    ctx->measure_exact = save_exact;

    if (!ctx->measure_only) {
        cr_draw_rrect(ctx, x, y, max_x - x, end_y - y,
                      ctx->_quote_bg, t->radius);
        cr_draw_rrect(ctx, x, y + t->radius * 0.5f, bar_w,
                      (end_y - y) - t->radius, ctx->_quote_bar, bar_w * 0.5f);
        quote_layout(b, ctx, inner_x, body_y, inner_max_x);
    }
    return end_y + t->unit * 0.55f;
}

/* Lay out (and, unless ctx->measure_only, draw) a callout's title row + body,
 * returning the y just below the content. Factored out so render_callout can
 * run a measure pass to size the card, paint the background, then re-run in
 * draw mode so the content lands on top of (and nested cards behind) the bg. */
/* Live open/closed state of a foldable block, seeded once from the source's
 * "-"/"+" marker and owned by the reader's clicks after that. Blocks with no
 * marker are not foldable and always report open. */
static bool fold_is_open(const MdBlock *b) {
    if (!b->callout_fold) return true;
    if (!b->fold_seen) {
        MdBlock *m = (MdBlock *)b;   /* the doc is mutable; render_block is not */
        m->fold_open = (b->callout_fold == 1) ? 0 : 1;
        m->fold_seen = 1;
    }
    return b->fold_open != 0;
}

static void fold_record_rect(MdRenderCtx *ctx, const MdBlock *b,
                             f32 x, f32 y, f32 w, f32 h) {
    if (ctx->measure_only || !b->callout_fold) return;
    if (!ctx->fold_rects || !ctx->fold_rect_count) return;
    if (*ctx->fold_rect_count >= ctx->fold_rect_cap) return;
    ctx->fold_rects[(*ctx->fold_rect_count)++] =
        (MdFoldRect){ x, y, w, h, (MdBlock *)b };
}

static f32 callout_layout(const MdBlock *b, MdRenderCtx *ctx,
                          f32 inner_x, f32 start_y, f32 max_x, Color title_fg) {
    const MdTypo *t = &ctx->_t;
    f32 cw = t->cw, ch = t->lh;
    f32 line_y = start_y + t->pad_y;

    /* Title row */
    const char *gly = callout_glyph((MdCalloutKind)b->callout_kind);
    cr_push_chrome(ctx, inner_x, line_y, (u32)gly[0], title_fg);
    f32 title_text_x = inner_x + cw * 2.0f;
    InlineRender tir = ir_make(ctx, line_y, title_text_x, max_x - 8, cw, ch);
    tir.fg = title_fg;
    tir.fg_bright = color_emphasize(title_fg, ctx->theme->bg, 0.2f);
    if (b->title_inline_count && b->title_inlines) {
        /* The label is markdown: "[!note] See [[Other]] for **details**"
         * renders as a link and bold text, not as its own punctuation. */
        ir_emit_runs(&tir, b->title_inlines, b->title_inline_count,
                     title_fg, false, false);
    } else if (b->info.len) {
        ir_emit_bytes(&tir, b->info, title_fg, false, false);
    } else {
        /* default title from callout kind */
        const char *defaults[MD_CALLOUT__COUNT] = {
            "Note", "Info", "Tip", "Success", "Warning", "Danger",
            "Question", "Example", "Quote", "Abstract", "Todo",
            "Bug", "Failure"
        };
        const char *def = defaults[b->callout_kind < MD_CALLOUT__COUNT ?
                                   b->callout_kind : 0];
        MdSlice s = { (const u8 *)def, (u32)strlen(def) };
        ir_emit_bytes(&tir, s, title_fg, false, false);
    }
    /* Chevron on the right for a foldable callout, plus the click target that
     * covers the whole title row — a 12-pixel arrow is not something anyone
     * should have to hit. */
    bool open = fold_is_open(b);
    if (b->callout_fold) {
        const char *chev = open ? "\xe2\x96\xbe" : "\xe2\x96\xb8";   /* ▾ ▸ */
        MdSlice cs = { (const u8 *)chev, 3 };
        InlineRender cir = ir_make(ctx, line_y, max_x - t->pad_x - cw,
                                   max_x + cw, cw, ch);
        cir.glyph_buf = NULL;              /* furniture, like the icon */
        ir_emit_bytes(&cir, cs, color_with_alpha(title_fg, 0.8f), false, false);
        fold_record_rect(ctx, b, inner_x - t->pad_x, line_y,
                         max_x - inner_x + t->pad_x, ch);
    }
    line_y = ir_finish(&tir);
    if (!open) return line_y + t->pad_y;

    /* Body — prefer structurally-parsed child blocks; fall back to a flat
     * inline run for callouts that parsed without children. */
    if (b->child_count) {
        line_y += t->unit * 0.30f;
        line_y = render_children(b, ctx, inner_x, line_y, max_x - t->pad_x);
        line_y -= t->unit * 0.5f;      /* drop the last child's trailing gap */
    } else if (b->inline_count) {
        line_y += t->unit * 0.30f;
        InlineRender bir = ir_make(ctx, line_y, inner_x, max_x - t->pad_x, cw, ch);
        ir_emit_inlines(&bir, b->inlines, b->inline_count);
        line_y = ir_finish(&bir);
    }
    return line_y + t->pad_y;
}

static f32 render_callout(const MdBlock *b, MdRenderCtx *ctx,
                           f32 x, f32 y, f32 max_x) {
    const MdTypo *t = &ctx->_t;
    Color accent = callout_color(ctx->theme, (MdCalloutKind)b->callout_kind);
    Color body_bg = color_with_alpha(accent, ctx->_is_light ? 0.10f : 0.13f);
    Color title_fg = accent;

    f32 bar_w   = fmaxf(3.0f, roundf(ctx->dpi * 2.0f));
    f32 inner_x = x + bar_w + t->pad_x * 1.4f;
    f32 start_y = y;
    f32 co_r = t->radius;

    /* Measure the content first (no draws) to size the card. measure_exact
     * forces nested code blocks to report their wrapped height so the card
     * doesn't clip code that soft-wraps inside the callout. */
    bool save_measure = ctx->measure_only;
    bool save_exact   = ctx->measure_exact;
    ctx->measure_only  = true;
    ctx->measure_exact = true;
    f32 end_y = callout_layout(b, ctx, inner_x, start_y, max_x, title_fg);
    ctx->measure_only  = save_measure;
    ctx->measure_exact = save_exact;

    if (!ctx->measure_only) {
        /* Background + accent bar first, then content on top. Nested callouts'
         * own backgrounds queue after this one, so they stack correctly. */
        cr_draw_rrect(ctx, x, start_y, max_x - x, end_y - start_y, body_bg, co_r);
        cr_draw_rrect(ctx, x, start_y + co_r, bar_w, end_y - start_y - 2 * co_r,
                      accent, bar_w * 0.5f);
        callout_layout(b, ctx, inner_x, start_y, max_x, title_fg);
    }
    return end_y + t->unit * 0.62f;
}

/* Thematic break: a short centred rule with real air around it. A full-bleed
 * line is a divider; this is a section pause, and it should read as one. */
/* <details> — a disclosure card. Shares the callout's measure-then-draw
 * contract and the same fold state, so clicking either behaves identically. */
static f32 render_details(const MdBlock *b, MdRenderCtx *ctx,
                          f32 x, f32 y, f32 max_x) {
    const MdTypo *t = &ctx->_t;
    bool open = fold_is_open(b);
    f32 bar_w   = fmaxf(2.0f, roundf(ctx->dpi * 1.5f));
    f32 inner_x = x + bar_w + t->pad_x * 1.4f;
    f32 head_y  = y + t->pad_y;
    f32 body_y  = head_y + t->lh + t->unit * 0.25f;
    f32 inner_max_x = max_x - t->pad_x;

    f32 end_y;
    if (!open || b->child_count == 0) {
        end_y = head_y + t->lh + t->pad_y;
    } else {
        bool save_measure = ctx->measure_only;
        bool save_exact   = ctx->measure_exact;
        ctx->measure_only  = true;
        ctx->measure_exact = true;
        f32 e = render_children(b, ctx, inner_x, body_y, inner_max_x)
              - t->unit * 0.5f;
        ctx->measure_only  = save_measure;
        ctx->measure_exact = save_exact;
        if (e < body_y) e = body_y + t->lh;
        end_y = e + t->pad_y;
    }

    if (!ctx->measure_only) {
        cr_draw_rrect(ctx, x, y, max_x - x, end_y - y, ctx->_quote_bg, t->radius);
        cr_draw_rrect(ctx, x, y + t->radius * 0.5f, bar_w,
                      (end_y - y) - t->radius, ctx->_quote_bar, bar_w * 0.5f);
        const char *chev = open ? "\xe2\x96\xbe " : "\xe2\x96\xb8 ";
        InlineRender hir = ir_make(ctx, head_y, inner_x, inner_max_x, t->cw, t->lh);
        MdSlice cs = { (const u8 *)chev, 4 };
        /* The chevron is furniture: drawn on the label's line, kept out of
         * the selection list. */
        MdGlyphRect *label_buf = hir.glyph_buf;
        hir.glyph_buf = NULL;
        ir_emit_bytes(&hir, cs, ctx->_muted, false, false);
        hir.glyph_buf = label_buf;
        if (b->title_inline_count && b->title_inlines)
            ir_emit_runs(&hir, b->title_inlines, b->title_inline_count,
                         ctx->_fg_bright, false, false);
        else
            ir_emit_bytes(&hir, b->info, ctx->_fg_bright, false, false);
        fold_record_rect(ctx, b, x, head_y, max_x - x, t->lh);
        if (open && b->child_count)
            render_children(b, ctx, inner_x, body_y, inner_max_x);
    }
    return end_y + t->unit * 0.62f;
}

static f32 render_hr(MdRenderCtx *ctx, f32 x, f32 y, f32 max_x) {
    const MdTypo *t = &ctx->_t;
    f32 gap = t->unit * 0.9f;
    f32 w   = fminf(max_x - x, roundf(t->cw * 8.0f));
    f32 rx  = x + ((max_x - x) - w) * 0.5f;
    cr_draw_rrect(ctx, rx, y + gap, w, t->hair * 2.0f,
                  ctx->_rule, t->hair);
    return y + gap * 2.0f + t->hair * 2.0f;
}

/* Index of the ':' that separates a YAML key from its value on [start,end):
 * the first ':' followed by whitespace or end-of-line. Returns -1 if none —
 * an inline ':' inside a value (URL, time) is not a key separator. */
static i32 fm_key_colon(const u8 *d, u32 start, u32 end) {
    for (u32 k = start; k < end; k++) {
        if (d[k] == ':' &&
            (k + 1 >= end || d[k + 1] == ' ' || d[k + 1] == '\t'))
            return (i32)k;
    }
    return -1;
}

static f32 render_frontmatter(const MdBlock *b, MdRenderCtx *ctx,
                              f32 x, f32 y, f32 max_x) {
    const MdTypo *t = &ctx->_t;
    f32 cw = t->cw;
    f32 ch = roundf(t->gh * MD_CODE_LEADING);   /* metadata rows, not prose */
    /* count lines */
    u32 lines = 1;
    for (u32 i = 0; i < b->raw.len; i++) if (b->raw.data[i] == '\n') lines++;
    f32 panel_h = (f32)lines * ch + 2.0f * t->pad_y;
    Color panel_bg = color_with_alpha(ctx->theme->ansi[8], 0.16f);
    if (!ctx->measure_only) renderer_set_ui_scale(ctx->r, cw, ch);
    cr_draw_rrect(ctx, x - t->pad_x, y, max_x - x + 2 * t->pad_x, panel_h,
                  panel_bg, t->radius);

    /* "Properties" badge — rounded pill */
    const char *label = "Properties";
    f32 label_w = (f32)strlen(label) * cw + 8;
    cr_draw_rrect(ctx, max_x - label_w, y + 4, label_w, ch,
                       color_with_alpha(ctx->theme->ansi[8], 0.4f), ch * 0.5f);
    {
        InlineRender ir = ir_make(ctx, y + 4, max_x - label_w + 4,
                                  max_x + 4, cw, ch);
        ir.glyph_buf = NULL;               /* a badge, not the note's text */
        ir.fg = ctx->theme->ansi[6];
        MdSlice ls = { (const u8 *)label, (u32)strlen(label) };
        ir_emit_bytes(&ir, ls, ir.fg, false, false);
    }

    /* Key/value rows ("Properties"). A YAML key terminates at a ':'
     * that is followed by whitespace or end-of-line — so an inline ':' inside
     * a value (e.g. an "http://" URL or a "12:00" time) is NOT mistaken for the
     * key separator. List-continuation lines ("  - item") and lines with no
     * key-colon render in the value column. The key column auto-sizes to the
     * widest key (clamped) so long keys aren't silently clipped against a
     * fixed gutter. */
    f32 line_y = y + t->pad_y;
    Color key_fg = color_with_alpha(ctx->_link, 0.95f);
    Color val_fg = ctx->_muted;

    /* First pass: widest key (in cells) among real key:value rows. */
    u32 max_key_cells = 4;
    {
        u32 i = 0;
        while (i < b->raw.len) {
            u32 ls = i;
            while (i < b->raw.len && b->raw.data[i] != '\n') i++;
            u32 le = i;
            u32 nb = ls; while (nb < le && b->raw.data[nb] == ' ') nb++;
            bool is_list = (nb < le && b->raw.data[nb] == '-');
            if (!is_list) {
                i32 kc = fm_key_colon(b->raw.data, ls, le);
                if (kc >= 0) {
                    u32 cells = 0;
                    for (u32 j = ls; j < (u32)kc; ) {
                        u32 cp = 0; u32 n = utf8_decode(b->raw.data + j, (u32)kc - j, &cp);
                        if (n == 0) { j++; continue; }
                        cells++; j += n;
                    }
                    if (cells > max_key_cells) max_key_cells = cells;
                }
            }
            if (i < b->raw.len) i++;
        }
    }
    if (max_key_cells > 24) max_key_cells = 24;   /* clamp the gutter width */
    f32 key_col_w = (f32)max_key_cells * cw;
    f32 val_x = x + key_col_w + cw;

    u32 i = 0;
    while (i < b->raw.len) {
        u32 line_start = i;
        while (i < b->raw.len && b->raw.data[i] != '\n') i++;
        u32 line_end = i;
        font_warm_text_glyphs_n(&ctx->r->font,
                                (const char *)(b->raw.data + line_start),
                                (usize)(line_end - line_start));
        u32 nb = line_start;
        while (nb < line_end && b->raw.data[nb] == ' ') nb++;
        bool is_list = (nb < line_end && b->raw.data[nb] == '-');
        i32 kc = is_list ? -1 : fm_key_colon(b->raw.data, line_start, line_end);
        if (kc >= 0) {
            /* key column — ellipsize if it would overflow the gutter */
            f32 key_limit = x + key_col_w;
            f32 cx = x;
            for (u32 j = line_start; j < (u32)kc; ) {
                u32 cp = 0; u32 n = utf8_decode(b->raw.data + j, (u32)kc - j, &cp);
                if (n == 0) { j++; continue; }
                /* Ellipsize only when this glyph would actually overflow the
                 * gutter. A test one cell early would cut the last character off
                 * the WIDEST key — the very one the gutter was sized to fit. */
                if (cx + cw > key_limit) { cr_push_glyph(ctx, cx, line_y, 0x2026, key_fg); break; }
                if (cp >= 32) cr_push_glyph(ctx, cx, line_y, cp, key_fg);
                cx += cw; j += n;
            }
            /* value column (skip ':' + leading spaces/tabs) */
            u32 vstart = (u32)kc + 1;
            while (vstart < line_end &&
                   (b->raw.data[vstart] == ' ' || b->raw.data[vstart] == '\t')) vstart++;
            /* The colon is laid out as the gap between the two columns. In
             * the text it is still a colon, and the value a space after it. */
            cr_note_glyph(ctx, cx, line_y, 0.0f, ':', 0);
            u8 gap = MD_GLYPH_GAP;
            f32 vx = val_x;
            for (u32 j = vstart; j < line_end && vx < max_x; ) {
                u32 cp = 0; u32 n = utf8_decode(b->raw.data + j, line_end - j, &cp);
                if (n == 0) { j++; continue; }
                if (cp >= 32) { cr_push_glyph_f(ctx, vx, line_y, cp, val_fg, gap); gap = 0; }
                vx += cw; j += n;
            }
        } else {
            /* list item / continuation — render whole line in the value column */
            f32 vx = val_x;
            for (u32 j = line_start; j < line_end && vx < max_x; ) {
                u32 cp = 0; u32 n = utf8_decode(b->raw.data + j, line_end - j, &cp);
                if (n == 0) { j++; continue; }
                if (cp >= 32) cr_push_glyph(ctx, vx, line_y, cp, val_fg);
                vx += cw; j += n;
            }
        }
        line_y += ch;
        if (i < b->raw.len) i++;
    }
    if (!ctx->measure_only) renderer_set_ui_scale(ctx->r, t->cw, t->lh);
    return y + panel_h + t->unit * 0.8f;
}

static u32 table_cell_text_cells(const MdTableCell *cell) {
    u32 cells = 0;
    if (!cell) return 0;
    for (u32 k = 0; k < cell->inline_count; k++) {
        cells += (u32)utf8_display_cols(cell->inlines[k].text.data,
                               cell->inlines[k].text.len);
        if (cell->inlines[k].kind == MD_INLINE_TAG) cells += 1;
    }
    return cells;
}

static f32 table_cell_text_x(char align, f32 cell_x, f32 col_px,
                             f32 text_w, f32 cw) {
    f32 tx = cell_x + cw;
    if (align == MD_ALIGN_CENTER) {
        tx = cell_x + (col_px - text_w) * 0.5f;
    } else if (align == MD_ALIGN_RIGHT) {
        tx = cell_x + col_px - text_w - cw;
    }
    if (tx < cell_x + cw * 0.5f) tx = cell_x + cw * 0.5f;
    return tx;
}

static f32 table_cell_content_height(const MdTableCell *cell, MdRenderCtx *ctx,
                                     f32 text_y, f32 tx, f32 max_x,
                                     f32 cw, f32 ch, bool header) {
    InlineRender ir = ir_make(ctx, text_y, tx, max_x, cw, ch);
    ir.measure_only = true;
    if (header) {
        ir.fg = color_emphasize(ctx->theme->fg, ctx->theme->bg, 0.15f);
        ir.fg_bright = color_emphasize(ir.fg, ctx->theme->bg, 0.15f);
    }
    ir_emit_inlines(&ir, cell->inlines, cell->inline_count);
    f32 h = ir_finish(&ir) - text_y;
    return h < ch ? ch : h;
}

/* Displayed width of one inline run, in cells.
 *
 * This has to agree with what ir_emit_runs actually DRAWS, and for the link
 * family that is not `text`: a wikilink keeps its label in `alt`/`url` and an
 * embed in `alt`/`url` too, so measuring `text` would report zero, and a
 * column of wikilinks would size itself to its header alone and wrap its
 * cells one character per line. */
static u32 inline_display_cols(const MdInline *in) {
    switch (in->kind) {
    case MD_INLINE_WIKILINK: {
        /* "[[#Heading]]" displays the anchor alone; an anchored link with no
         * alias displays "Note › Heading" (3 cells for the separator). */
        if (in->url.len == 0 && in->anchor.len)
            return (u32)utf8_display_cols(in->anchor.data, in->anchor.len);
        MdSlice d = in->alt.len ? in->alt : in->url;
        u32 n = (u32)utf8_display_cols(d.data, d.len);
        if (in->alt.len == 0 && in->anchor.len)
            n += 3 + (u32)utf8_display_cols(in->anchor.data, in->anchor.len);
        return n;
    }
    case MD_INLINE_EMBED:
    case MD_INLINE_IMAGE: {
        MdSlice d = in->alt.len ? in->alt : in->url;
        return (u32)utf8_display_cols(d.data, d.len) + 2;   /* chip label + gap */
    }
    case MD_INLINE_AUTOLINK:
    case MD_INLINE_CODE:
        return (u32)utf8_display_cols(in->text.data, in->text.len);
    case MD_INLINE_TAG:
        return (u32)utf8_display_cols(in->text.data, in->text.len) + 1;   /* leading '#' */
    default:
        return (u32)utf8_display_cols(in->text.data, in->text.len);
    }
}

static f32 render_table(const MdBlock *b, MdRenderCtx *ctx,
                         f32 x, f32 y, f32 max_x) {
    const MdTypo *t = &ctx->_t;
    f32 cw = t->cw, ch = t->lh;
    /* b->cells is NULL when the parser's cell-grid arena allocation failed
     * (it still emits the table block with cols/rows set). Guard before the
     * b->cells[...] dereference below. */
    if (b->cols == 0 || b->rows == 0 || !b->cells) return y + ch;

    /* Compute per-column natural widths (in cells) by walking inline runs.
     * Approximation: codepoint count per cell. */
    u32 col_w[32] = {0};
    for (u16 r = 0; r < b->rows; r++) {
        for (u16 c = 0; c < b->cols && c < 32; c++) {
            const MdTableCell *cell = &b->cells[(usize)r * b->cols + c];
            u32 w = 0;
            for (u32 k = 0; k < cell->inline_count; k++)
                w += inline_display_cols(&cell->inlines[k]);
            if (w > col_w[c]) col_w[c] = w;
        }
    }
    /* Keep a readable minimum; crowded tables scroll instead of losing
     * columns to the viewer's scissor or splitting every word in thirds. */
    f32 viewport_x = x, viewport_w = fmaxf(cw, max_x - x);
    u32 minimum[32] = {0};
    u32 sum = 0, min_sum = 0;
    for (u16 c = 0; c < b->cols && c < 32; c++) {
        if (col_w[c] > 40) col_w[c] = 40;
        minimum[c] = col_w[c] < 8 ? col_w[c] : 8;
        if (minimum[c] < 1) minimum[c] = 1;
        if (col_w[c] < minimum[c]) col_w[c] = minimum[c];
        sum += col_w[c] + 2; min_sum += minimum[c] + 2;
    }
    f32 avail = viewport_w / cw;
    if (sum > avail && min_sum < avail && sum > min_sum) {
        f32 ratio = (avail - min_sum) / (sum - min_sum);
        for (u16 c = 0; c < b->cols && c < 32; c++)
            col_w[c] = minimum[c] + (u32)((col_w[c] - minimum[c]) * ratio);
    } else if (sum > avail) {
        for (u16 c = 0; c < b->cols && c < 32; c++) col_w[c] = minimum[c];
    }
    f32 natural_w = 0;
    for (u16 c = 0; c < b->cols && c < 32; c++) natural_w += (col_w[c] + 2) * cw;
    f32 overflow = fmaxf(0.0f, natural_w - viewport_w);
    f32 offset = fminf(fmaxf(0.0f, ctx->table_scroll_x), overflow);
    if (overflow > 0) {
        if (!ctx->measure_only && ctx->table_overflow && overflow > *ctx->table_overflow)
            *ctx->table_overflow = overflow;
        InlineRender hint = ir_make(ctx, y, viewport_x, max_x, cw, ch);
        hint.glyph_buf = NULL; hint.glyph_count = NULL;
        const char *msg = "Shift+wheel: scroll table";
        ir_emit_bytes(&hint, (MdSlice){(const u8 *)msg, (u32)strlen(msg)}, ctx->theme->fg, false, false);
        y = ir_finish(&hint);
    }
    x -= offset;
    max_x = x + natural_w;

    /* Geometry pre-pass: the total table extent is needed to draw the outer
     * frame in a single rect before cells get filled in. */
    f32 cell_pad_y = t->pad_y;            /* vertical breathing room per row */
    f32 sep_thick  = t->hair;
    f32 head_under = fmaxf(1.5f, roundf(ctx->dpi));
    f32 frame_th   = t->hair;

    /* Compute total content width (sum of columns) so the outer frame
     * and vertical separators can be clamped to the actual table extent
     * rather than the full viewport. */
    f32 total_w = 0.0f;
    for (u16 c = 0; c < b->cols && c < 32; c++) total_w += (col_w[c] + 2) * cw;
    if (total_w > max_x - x) total_w = max_x - x;

    /* Row-height pre-pass. Cells soft-wrap when the viewer gets narrow, so
     * table rows must expand with the tallest wrapped cell. Otherwise the
     * following row starts at the old single-line baseline and text overlaps. */
    f32 header_h = ch + cell_pad_y;
    f32 body_h = 0.0f;
    for (u16 r = 0; r < b->rows; r++) {
        f32 row_content_h = ch;
        f32 cell_x = x;
        for (u16 c = 0; c < b->cols && c < 32; c++) {
            const MdTableCell *cell = &b->cells[(usize)r * b->cols + c];
            f32 col_px = (col_w[c] + 2) * cw;
            char align = MD_ALIGN_LEFT;
            if (b->info.len > c) align = (char)b->info.data[c];

            f32 text_w = (f32)table_cell_text_cells(cell) * cw;
            f32 tx = table_cell_text_x(align, cell_x, col_px, text_w, cw);
            f32 text_max_x = cell_x + col_px - cw * 0.5f;
            f32 text_h = table_cell_content_height(cell, ctx, 0.0f, tx,
                                                   text_max_x, cw, ch,
                                                   r == 0);
            if (text_h > row_content_h) row_content_h = text_h;
            cell_x += col_px;
        }

        f32 row_h = row_content_h + cell_pad_y;
        if (r == 0) header_h = row_h;
        else        body_h += row_h;
    }

    /* Header row sits at top + has a slightly tinted background. */
    f32 head_y0 = y;
    f32 head_y1 = y + header_h;
    f32 body_y0 = head_y1 + head_under;
    f32 body_y1 = body_y0 + body_h;

    /* Off-screen: the row-height pre-pass above already produced the full
     * table extent — return it and skip the frame/separator/cell draw passes
     * (mirrors render_code's measure_only fast path). */
    if (ctx->measure_only) {
        f32 tbl_h_m = header_h + head_under + body_h;
        return y + tbl_h_m + t->unit * 0.72f;
    }

    /* Color palette derived from the theme. The border is used at full
     * alpha so the lines actually pop on dark themes (where border alpha=1
     * already but its luminance is low, so it is boosted slightly for
     * contrast). */
    Color line_c = ctx->theme->border;
    line_c.a = 1.0f;
    /* Brighten by adding a touch of fg so very-dark borders don't vanish
     * on near-black backgrounds. */
    Color line_strong = {
        line_c.r * 0.55f + ctx->theme->fg.r * 0.45f,
        line_c.g * 0.55f + ctx->theme->fg.g * 0.45f,
        line_c.b * 0.55f + ctx->theme->fg.b * 0.45f,
        1.0f
    };
    Color line_dim = color_with_alpha(line_strong, 0.45f);
    Color head_bg  = color_with_alpha(ctx->_surface_raised,
                                      ctx->_is_light ? 0.75f : 0.45f);
    Color head_underline_c = color_with_alpha(line_strong, 0.85f);

    /* --- Background & rules ------------------------------------------------
     * Horizontal rules only. Boxing every cell (outer frame + a rule between
     * each column) turns a small table into a grid of cages and fights the
     * text; the row rhythm alone is enough to keep columns readable. */
    f32 tbl_h = (head_y1 - head_y0) + head_under + (body_y1 - body_y0);
    cr_draw_rrect(ctx, x, head_y0, total_w, header_h, head_bg,
                  t->radius * 0.6f);
    cr_draw_rect(ctx, x, head_y1, total_w, head_under, head_underline_c);
    cr_draw_rect(ctx, x, head_y0 + tbl_h - frame_th, total_w, frame_th,
                 color_with_alpha(line_strong, 0.5f));

    /* Subtle horizontal separators between body rows. */
    {
        f32 sy = body_y0;
        for (u16 r = 1; r + 1 < b->rows; r++) {
            f32 row_content_h = ch;
            f32 cell_x = x;
            for (u16 c = 0; c < b->cols && c < 32; c++) {
                const MdTableCell *cell = &b->cells[(usize)r * b->cols + c];
                f32 col_px = (col_w[c] + 2) * cw;
                char align = MD_ALIGN_LEFT;
                if (b->info.len > c) align = (char)b->info.data[c];
                f32 text_w = (f32)table_cell_text_cells(cell) * cw;
                f32 tx = table_cell_text_x(align, cell_x, col_px, text_w, cw);
                f32 text_h = table_cell_content_height(cell, ctx, 0.0f, tx,
                                                       cell_x + col_px - cw * 0.5f,
                                                       cw, ch, false);
                if (text_h > row_content_h) row_content_h = text_h;
                cell_x += col_px;
            }
            sy += row_content_h + cell_pad_y;
            cr_draw_rect(ctx, x, sy, total_w, sep_thick, line_dim);
        }
    }

    /* --- Cell content ------------------------------------------------------ */
    f32 row_y = head_y0;
    for (u16 r = 0; r < b->rows; r++) {
        f32 text_y = row_y + cell_pad_y * 0.5f;
        f32 row_content_h = ch;
        f32 cell_x = x;
        for (u16 c = 0; c < b->cols && c < 32; c++) {
            const MdTableCell *cell = &b->cells[(usize)r * b->cols + c];
            f32 col_px = (col_w[c] + 2) * cw;
            char align = MD_ALIGN_LEFT;
            if (b->info.len > c) align = (char)b->info.data[c];

            f32 text_w = (f32)table_cell_text_cells(cell) * cw;
            f32 tx = table_cell_text_x(align, cell_x, col_px, text_w, cw);
            f32 text_max_x = cell_x + col_px - cw * 0.5f;
            f32 text_h = table_cell_content_height(cell, ctx, 0.0f, tx,
                                                   text_max_x, cw, ch,
                                                   r == 0);
            if (text_h > row_content_h) row_content_h = text_h;

            InlineRender ir = ir_make(ctx, text_y, tx,
                                      text_max_x, cw, ch);
            if (r == 0) {
                ir.fg = color_emphasize(ctx->theme->fg, ctx->theme->bg, 0.15f);
                ir.fg_bright = color_emphasize(ir.fg, ctx->theme->bg, 0.15f);
            }
            /* Where this cell starts in the text: a new line for a row's
             * first cell (its y says so), a tab for the others. A marker
             * rather than a flag on the cell's first glyph, because an empty
             * cell has no glyph and still has its place in the row. */
            ir.glyph_flags = c ? MD_GLYPH_CELL : 0;
            mr_note_glyph(&ir, tx, text_y, 0.0f, 0);
            ir_emit_inlines(&ir, cell->inlines, cell->inline_count);

            cell_x += col_px;
        }
        if (r == 0) row_y = body_y0;
        else        row_y += row_content_h + cell_pad_y;
    }
    return head_y0 + tbl_h + t->unit * 0.72f;
}

static f32 render_html(const MdBlock *b, MdRenderCtx *ctx,
                        f32 x, f32 y, f32 max_x) {
    /* Render passthrough as dim mono. */
    const MdTypo *t = &ctx->_t;
    f32 cw = t->cw;
    f32 ch = roundf(t->gh * MD_CODE_LEADING);
    if (!ctx->measure_only) renderer_set_ui_scale(ctx->r, cw, ch);
    Color fg = ctx->_muted;
    f32 line_y = y;
    u32 i = 0;
    while (i < b->raw.len) {
        u32 ls = i;
        while (i < b->raw.len && b->raw.data[i] != '\n') i++;
        f32 cx = x;
        u32 j = ls;
        while (j < i && cx < max_x) {
            u32 cp = 0;
            u32 n = utf8_decode(b->raw.data + j, i - j, &cp);
            if (n == 0) { j++; continue; }
            if (cp >= 32) cr_push_glyph(ctx, cx, line_y, cp, fg);
            cx += cw;
            j += n;
        }
        line_y += ch;
        if (i < b->raw.len) i++;
    }
    if (!ctx->measure_only) renderer_set_ui_scale(ctx->r, t->cw, t->lh);
    return line_y + t->unit * 0.5f;
}

/* Inline IMAGE/EMBED rendering: walk a paragraph's inline runs and, if
 * any are image/embed, draw the image at block-level dimensions and
 * suppress the inline placeholder. Returns the new y after image.
 * Images are detected by scanning the block's first IMAGE/EMBED inline.
 */
static f32 render_image_block(const MdInline *img, MdRenderCtx *ctx,
                               f32 x, f32 y, f32 max_x) {
    /* Media/document embed (mp4/mp3/pdf/…) → a labelled chip rather than an
     * attempted image decode (which would render "[image not found]"). */
    {
        MdSlice tgt = img->url.len ? img->url : img->alt;
        const char *mlabel = media_chip_label(tgt);
        if (mlabel) {
            f32 cw = ctx->_t.cw, ch = ctx->_t.lh;
            const u8 *nm = tgt.data; u32 nl = tgt.len;
            for (i32 k = (i32)tgt.len - 1; k >= 0; k--)
                if (tgt.data[k] == '/') { nm = tgt.data + k + 1; nl = tgt.len - (u32)(k + 1); break; }
            u32 lbl = (u32)strlen(mlabel);
            /* Cap the basename so the whole chip fits on one line — the chip
             * height is fixed at one row, so a wrapped name would overlap the
             * next block. (utf8-naive truncation; good enough for filenames.) */
            u32 avail = md_cells_fit(max_x - x, cw);
            if (avail < lbl + 6) avail = lbl + 6;
            u32 max_name = avail - lbl - 4;
            if (nl > max_name) nl = max_name;
            f32 chip_w = (f32)(lbl + 2 + nl + 2) * cw;
            if (chip_w > max_x - x) chip_w = max_x - x;
            f32 chip_h = roundf(ch * 1.35f);
            if (!ctx->measure_only) {
                cr_draw_rrect(ctx, x, y, chip_w, chip_h,
                              color_with_alpha(ctx->theme->ansi[5], 0.16f),
                              ctx->_t.radius);
                InlineRender ir = ir_make(ctx, y + (chip_h - ch) * 0.5f,
                                          x + cw, x + chip_w + cw, cw, ch);
                ir_link_begin_anchored(&ir, tgt, MD_LINKRECT_EMBED, img->anchor,
                                       img->anchor_is_block != 0);
                MdSlice ls = { (const u8 *)mlabel, lbl };
                ir_emit_bytes(&ir, ls, ctx->theme->ansi[13], false, false);
                ir.cx += cw;   /* gap */
                ir.glyph_flags |= MD_GLYPH_GAP;
                MdSlice ns = { nm, nl };
                ir_emit_bytes(&ir, ns, color_dim(ctx->theme->fg, 0.85f), false, false);
                ir_link_end(&ir);
            }
            return y + chip_h + ctx->_t.unit * 0.6f;
        }
    }
    if (!ctx->images || img->url.len == 0) {
        /* placeholder badge */
        InlineRender ir = ir_make(ctx, y, x, max_x, ctx->_t.cw, ctx->_t.lh);
        ir.fg = ctx->_muted;
        const char *p = "[image]";
        MdSlice s = { (const u8 *)p, (u32)strlen(p) };
        ir_emit_bytes(&ir, s, ir.fg, false, false);
        return ir_finish(&ir) + ctx->_t.unit * 0.6f;
    }
    /* Measure pass: layout needs the on-screen box, nothing else. Ask for the
     * dimensions instead of the pixels.
     *
     * md_image_cache_get here would fully decode (premultiply, downscale,
     * cache) EVERY image of a document on its first frame, on-screen or not —
     * linear in document length before anything appears. It would also invert
     * the cache's LRU order: the walk runs in document order, so the
     * least-recently-used entry would be the image at the top of the file,
     * which is exactly the one on screen. */
    if (ctx->measure_only) {
        i32 mw = 0, mh = 0;
        if (md_image_cache_dims(ctx->images, ctx->base_dir,
                                (const char *)img->url.data, img->url.len,
                                &mw, &mh) && mw > 0 && mh > 0) {
            f32 mmw, mmh;
            if (img->img_w) {
                mmw = (f32)img->img_w * ctx->dpi;
                mmh = img->img_h ? (f32)img->img_h * ctx->dpi
                                 : mmw * (f32)mh / (f32)mw;
                f32 avail = max_x - x;
                if (mmw > avail) { mmh *= avail / mmw; }
            } else {
                f32 mmax_w = fminf((max_x - x), 480.0f * ctx->dpi);
                f32 mscale = mmax_w / (f32)mw;
                if (mscale > 1.0f) mscale = 1.0f;
                mmh = (f32)mh * mscale;
            }
            return y + mmh + ctx->_t.unit * 0.6f;
        }
        /* Unresolvable: fall through so the height matches the
         * "[image not found: …]" line the visible pass will draw. The failure
         * is memoised in the cache, so the get() below is a cheap hit. */
    }

    const MdImageEntry *e = ctx->measure_only && ctx->images->async_headers
        ? md_image_cache_peek(ctx->images, ctx->base_dir,
                             (const char *)img->url.data, img->url.len)
        : md_image_cache_get(ctx->images, ctx->base_dir,
                                               (const char *)img->url.data,
                                               img->url.len);
    if (!e || e->failed || !e->rgba) {
        bool loading = e && e->loading && !e->failed;
        Color fg = loading ? color_with_alpha(ctx->theme->fg, 0.55f)
                           : color_with_alpha(ctx->theme->ansi[9], 0.85f);
        InlineRender ir = ir_make(ctx, y, x, max_x, ctx->_t.cw, ctx->_t.lh);
        ir.fg = fg;
        const char *prefix = loading ? "[loading image: " : "[image not found: ";
        MdSlice ps = { (const u8 *)prefix, (u32)strlen(prefix) };
        ir_link_begin(&ir, img->url, MD_LINKRECT_IMAGE);
        ir_emit_bytes(&ir, ps, fg, false, false);
        ir_emit_bytes(&ir, img->url, fg, false, false);
        const char *suffix = "]";
        MdSlice ss = { (const u8 *)suffix, 1 };
        ir_emit_bytes(&ir, ss, fg, false, false);
        ir_link_end(&ir);
        return ir_finish(&ir) + ctx->_t.unit * 0.6f;
    }
    /* Explicit size wins — `![[img.png|300]]` / `![[img.png|640x480]]` means
     * "draw it this big". Still clamped to the column so an oversized value
     * can't bleed out of the measure. */
    f32 w, h;
    if (img->img_w) {
        w = (f32)img->img_w * ctx->dpi;
        h = img->img_h ? (f32)img->img_h * ctx->dpi
                       : w * (f32)e->h / (f32)(e->w ? e->w : 1);
        f32 avail = max_x - x;
        if (w > avail) { h *= avail / w; w = avail; }
    } else {
        /* Fit to the column keeping aspect ratio, capped at 480 pt. */
        f32 max_w = fminf((max_x - x), 480.0f * ctx->dpi);
        f32 scale = max_w / (f32)e->w;
        if (scale > 1.0f) scale = 1.0f;
        w = (f32)e->w * scale;
        h = (f32)e->h * scale;
    }
    /* Off-screen (measure pass): only the height matters. Skip the texture
     * bind + draw + batch-break that a hidden image would otherwise cost
     * every frame. renderer_draw_image_cached already flushes rects+glyphs
     * at entry, so no caller-side flush is needed for the visible path. */
    if (ctx->measure_only)
        return y + h + ctx->_t.unit * 0.6f;
    /* generation = e->generation (unique per rgba load), NOT 0: the MdImageEntry
     * address is recycled across document switches, so a constant generation let
     * the GPU image cache serve a prior document's texture for a reused address
     * with matching dimensions (ABA). The per-load id forces a re-upload. */
    renderer_draw_image_cached(ctx->r, e, e->generation, e->rgba, e->w, e->h, x, y, w, h);
    /* Click target spanning the rendered image so the user can open the
     * source file (or original URL) in the OS-default app / browser. */
    if (ctx->link_rects && ctx->link_rect_count &&
        *ctx->link_rect_count < ctx->link_rect_cap && img->url.len > 0) {
        ctx->link_rects[*ctx->link_rect_count] = (MdLinkRect){
            .x = x, .y = y, .w = w, .h = h,
            .url = img->url.data, .url_len = img->url.len,
            .kind = MD_LINKRECT_IMAGE,
        };
        (*ctx->link_rect_count)++;
    }
    return y + h + ctx->_t.unit * 0.6f;
}

/* Transclusion: `![[Note]]`, `![[Note#Heading]]`, `![[Note#^block]]`.
 *
 * The embedded document is parsed once and cached (md_embed.c); here it is
 * laid out as a card so the reader can tell borrowed content from the note
 * they are reading. Recursion is bounded by ctx->embed_depth — a note that
 * embeds a note that embeds the first one otherwise never returns.
 *
 * Sizing follows the same measure-then-draw contract as callouts: the card has
 * to be painted BEFORE the content that sits on it, and the content's height
 * is only known by laying it out. */
#define MD_EMBED_MAX_DEPTH 3

static f32 embed_body(const MdDoc *sub, u32 first, u32 count,
                      MdRenderCtx *ctx, f32 x, f32 y, f32 max_x) {
    f32 cy = y;
    bool save_nested = ctx->in_nested_block;
    ctx->in_nested_block = true;
    ctx->embed_depth++;
    for (u32 i = 0; i < count; i++) {
        const MdBlock *blk = &sub->blocks[first + i];
        u32 first_glyph = glyph_list_len(ctx);
        cy = render_block(blk, ctx, x, cy, max_x);
        block_mark_text(ctx, first_glyph, i ? blk - 1 : NULL, blk);
    }
    ctx->embed_depth--;
    ctx->in_nested_block = save_nested;
    return cy;
}

static f32 render_embed_block(const MdInline *em, MdRenderCtx *ctx,
                              f32 x, f32 y, f32 max_x) {
    const MdTypo *t = &ctx->_t;

    /* One line, dimmed, when the embed can't be honoured — a silent gap would
     * read as "the note is empty". */
    #define EMBED_NOTICE(msg) do {                                                    InlineRender ir = ir_make(ctx, y, x + t->pad_x, max_x, t->cw, t->lh);          ir.fg = ctx->_muted;                                                           MdSlice _s = { (const u8 *)(msg), (u32)strlen(msg) };                          ir_link_begin(&ir, em->url, MD_LINKRECT_EMBED);                                ir_emit_bytes(&ir, _s, ctx->_muted, false, false);                             ir_emit_bytes(&ir, em->url, ctx->_link, true, false);                          ir_link_end(&ir);                                                              return ir_finish(&ir) + t->unit * 0.6f;                                    } while (0)

    if (ctx->embed_depth >= MD_EMBED_MAX_DEPTH) EMBED_NOTICE("embed too deep: ");
    if (!ctx->embeds)                           EMBED_NOTICE("embed: ");

    char path[1024];
    MdEmbedStatus why = MD_EMBED_OK;
    const MdDoc *sub = md_embed_cache_get_ex(ctx->embeds, ctx->base_dir,
                                             em->url.data, em->url.len,
                                             path, sizeof path, &why);
    /* Two different lines: "not found" says fix the link, "too large" says
     * the note exists and this page will not carry it. A parse that did not
     * fit comes back as a document with no blocks, which would otherwise
     * render as an EMPTY card. */
    if (!sub && why == MD_EMBED_TOO_LARGE) EMBED_NOTICE("note too large to embed: ");
    if (!sub && why == MD_EMBED_LOADING) EMBED_NOTICE("loading note: ");
    if (!sub) EMBED_NOTICE("note not found: ");

    u32 first = 0, count = 0;
    if (!md_embed_section(sub, em->anchor.data, em->anchor.len,
                          em->anchor_is_block != 0, &first, &count) || count == 0)
        EMBED_NOTICE("section not found in: ");
    #undef EMBED_NOTICE

    f32 bar_w   = fmaxf(2.0f, roundf(ctx->dpi * 1.5f));
    f32 inner_x = x + bar_w + t->pad_x * 1.4f;
    f32 inner_max_x = max_x - t->pad_x;
    f32 title_y = y + t->pad_y;

    /* Title row: the source note, clickable, so a transclusion is also a way
     * in to the note it came from. */
    const u8 *nm = em->url.data;
    u32 nl = em->url.len;
    for (i32 k = (i32)em->url.len - 1; k >= 0; k--)
        if (em->url.data[k] == '/') { nm = em->url.data + k + 1;
                                      nl = em->url.len - (u32)(k + 1); break; }

    bool save_measure = ctx->measure_only;
    bool save_exact   = ctx->measure_exact;
    ctx->measure_only  = true;
    ctx->measure_exact = true;
    f32 head_end = title_y + t->lh;
    f32 end_y = embed_body(sub, first, count, ctx, inner_x,
                           head_end + t->unit * 0.15f, inner_max_x);
    end_y -= t->unit * 0.5f;              /* drop the last block's trailing gap */
    if (end_y < head_end) end_y = head_end;
    end_y += t->pad_y;
    ctx->measure_only  = save_measure;
    ctx->measure_exact = save_exact;

    if (!ctx->measure_only) {
        cr_draw_rrect(ctx, x, y, max_x - x, end_y - y, ctx->_quote_bg, t->radius);
        cr_draw_rrect(ctx, x, y + t->radius * 0.5f, bar_w,
                      (end_y - y) - t->radius, ctx->_quote_bar, bar_w * 0.5f);

        InlineRender tir = ir_make(ctx, title_y, inner_x, inner_max_x,
                                   t->cw, t->lh);
        tir.fg = ctx->_muted;
        MdSlice icon = { (const u8 *)"\xe2\x86\x97 ", 4 };   /* ↗ */
        ir_emit_bytes(&tir, icon, ctx->_muted, false, false);
        ir_link_begin_anchored(&tir, em->url, MD_LINKRECT_WIKILINK,
                               em->anchor, em->anchor_is_block != 0);
        MdSlice ns = { nm, nl };
        ir_emit_bytes(&tir, ns, ctx->_link, true, false);
        if (em->anchor.len) {
            const char *sep = em->anchor_is_block ? " \xc2\xb6 " : " \xe2\x80\xba ";
            MdSlice ss = { (const u8 *)sep, (u32)strlen(sep) };
            ir_emit_bytes(&tir, ss, color_with_alpha(ctx->_link, 0.65f), true, false);
            ir_emit_bytes(&tir, em->anchor, ctx->_link, true, false);
        }
        ir_link_end(&tir);

        embed_body(sub, first, count, ctx, inner_x,
                   head_end + t->unit * 0.15f, inner_max_x);
    }
    return end_y + t->unit * 0.62f;
}

/* Display math block ($$…$$ or a ```math fence). TeX is in b->raw. Centered. */
static f32 render_math_block(const MdBlock *b, MdRenderCtx *ctx,
                             f32 x, f32 y, f32 max_x) {
    f32 pad = 0.75f * ctx->_t.unit;
    f32 w = 0, asc = 0, desc = 0;
    md_math_measure(b->raw.data, b->raw.len, ctx->cw, ctx->ch, true, &w, &asc, &desc);
    if (w <= 0.0f)              /* nothing measurable → show the raw TeX as code */
        return render_code_source(b, ctx, x, y, max_x);
    if (!ctx->measure_only) {
        f32 avail = max_x - x;
        f32 fx = x + (avail > w ? (avail - w) * 0.5f : 0.0f);
        f32 baseline = y + pad + asc;
        md_math_render(ctx->r, b->raw.data, b->raw.len, fx, baseline,
                       ctx->cw, ctx->ch, true, ctx->_fg);
    }
    return y + pad + asc + desc + pad;
}

/* Native Mermaid diagram (```mermaid fence). Source is in b->raw. */
static f32 render_diagram_block(const MdBlock *b, MdRenderCtx *ctx,
                                f32 x, f32 y, f32 max_x) {
    f32 pad = 0.75f * ctx->_t.unit;
    f32 w = 0, h = 0;
    md_diagram_measure(b->raw.data, b->raw.len, max_x - x, ctx->cw, ctx->ch, &w, &h);
    if (w <= 0.0f || h <= 0.0f)        /* nothing laid out → raw code fallback */
        return render_code_source(b, ctx, x, y, max_x);
    if (!ctx->measure_only)
        md_diagram_render(ctx->r, ctx->theme, b->raw.data, b->raw.len,
                          x, y + pad, max_x - x, ctx->cw, ctx->ch);
    return y + pad + h + pad;
}

static f32 render_block(const MdBlock *b, MdRenderCtx *ctx,
                         f32 x, f32 y, f32 max_x) {
    /* A block continuing a list item is laid out under that item's TEXT
     * column, so a loose item's later paragraphs line up with its first one
     * instead of falling back to the bullet column. list_depth/list_ordered
     * on the continuation block describe the item it belongs to. */
    if (b->list_cont) {
        f32 marker_x, text_x;
        list_geometry(ctx, b, x, &marker_x, &text_x);
        x = text_x;
    }
    /* Media-only paragraph: every run is an image/embed or whitespace, so each
     * one is drawn as its own block, stacked.
     *
     * Consecutive image lines form a single paragraph whose runs are EMBED,
     * TEXT("\n"), EMBED, so a gallery counts as media-only too, not only a
     * paragraph of EXACTLY one inline. */
    if (b->kind == MD_BLOCK_PARAGRAPH && b->inline_count >= 1) {
        u32 media = 0;
        bool only_media = true;
        for (u32 i = 0; i < b->inline_count && only_media; i++) {
            const MdInline *r = &b->inlines[i];
            if (r->kind == MD_INLINE_IMAGE || r->kind == MD_INLINE_EMBED) { media++; continue; }
            if (r->kind == MD_INLINE_LINEBREAK) continue;
            if (r->kind == MD_INLINE_TEXT) {
                bool ws = true;
                for (u32 k = 0; k < r->text.len; k++) {
                    u8 c = r->text.data[k];
                    if (c != ' ' && c != '\t' && c != '\n' && c != '\r') { ws = false; break; }
                }
                if (ws) continue;
            }
            only_media = false;
        }
        if (only_media && media >= 1) {
            f32 cy = y;
            for (u32 i = 0; i < b->inline_count; i++) {
                const MdInline *r = &b->inlines[i];
                if (r->kind != MD_INLINE_IMAGE && r->kind != MD_INLINE_EMBED) continue;
                /* `![[Note]]` transcludes the note; `![[img.png]]` is still an
                 * image. The extension decides — everything the image pipeline
                 * can decode goes to it, anything else is a document. */
                if (r->kind == MD_INLINE_EMBED && embed_is_document(r->url))
                    cy = render_embed_block(r, ctx, x, cy, max_x);
                else
                    cy = render_image_block(r, ctx, x, cy, max_x);
            }
            return cy;
        }
    }

    /* A block whose entire content was a "^block-id" line has nothing left to
     * draw — and must not leave a paragraph-sized hole where it was. */
    if (b->inline_count == 0 && b->child_count == 0 && b->raw.len == 0 &&
        (b->kind == MD_BLOCK_PARAGRAPH || b->kind == MD_BLOCK_LIST_ITEM ||
         b->kind == MD_BLOCK_TASK_ITEM))
        return y;

    switch (b->kind) {
    case MD_BLOCK_FRONTMATTER: return render_frontmatter(b, ctx, x, y, max_x);
    case MD_BLOCK_HEADING:     return render_heading(b, ctx, x, y, max_x);
    case MD_BLOCK_PARAGRAPH:   return render_paragraph(b, ctx, x, y, max_x);
    case MD_BLOCK_CODE:        return render_code(b, ctx, x, y, max_x);
    case MD_BLOCK_LIST_ITEM:   return render_list_item(b, ctx, x, y, max_x);
    case MD_BLOCK_TASK_ITEM:   return render_task_item(b, ctx, x, y, max_x);
    case MD_BLOCK_QUOTE:       return render_quote(b, ctx, x, y, max_x);
    case MD_BLOCK_CALLOUT:     return render_callout(b, ctx, x, y, max_x);
    case MD_BLOCK_TABLE:       return render_table(b, ctx, x, y, max_x);
    case MD_BLOCK_HR:          return render_hr(ctx, x, y, max_x);
    case MD_BLOCK_HTML:        return render_html(b, ctx, x, y, max_x);
    case MD_BLOCK_MATH:        return render_math_block(b, ctx, x, y, max_x);
    case MD_BLOCK_DETAILS:     return render_details(b, ctx, x, y, max_x);
    case MD_BLOCK_DEF_TERM:    return render_def_term(b, ctx, x, y, max_x);
    case MD_BLOCK_DEF_DESC:    return render_def_desc(b, ctx, x, y, max_x);
    default:                   return y + ctx->_t.lh;
    }
}

/* =========================================================================
 * Public entry
 * ========================================================================= */

/* First usable text run of a heading's inline content, for the outline label.
 * Most headings are plain text (inlines[0]); fall back through emphasis
 * wrappers that nest their text in children. */
static MdSlice heading_label_slice(const MdBlock *b) {
    for (u32 i = 0; i < b->inline_count; i++) {
        const MdInline *r = &b->inlines[i];
        if (r->text.len) return r->text;
        /* Link/wikilink/image/embed runs keep their visible text in alt/url,
         * not text — fall back to those so a heading like `## [[Note]]` or
         * `## ![img](u)` still gets an outline label instead of a blank row. */
        if (r->alt.len) return r->alt;
        if (r->url.len) return r->url;
        if (r->child_count && r->children && r->children[0].text.len)
            return r->children[0].text;
    }
    return (MdSlice){ NULL, 0 };
}

/* Space that belongs to the JOIN between two blocks rather than to either one.
 * A list item can't carry it: the gap after the last item of a list and the
 * gap between two items are different sizes, and only the walker knows which
 * one it is looking at. */
static f32 block_lead(const MdBlock *prev, const MdBlock *cur, const MdTypo *t) {
    if (!prev || !cur) return 0.0f;
    /* A continuation block (the second paragraph of a loose item, a fence
     * indented under it) counts as part of the list: charging the "list
     * closes" gap before it and the "list opens" gap after it is what made
     * one loose list read as three separate ones. */
    bool prev_list = prev->kind == MD_BLOCK_LIST_ITEM ||
                     prev->kind == MD_BLOCK_TASK_ITEM || prev->list_cont;
    bool cur_list  = cur->kind == MD_BLOCK_LIST_ITEM ||
                     cur->kind == MD_BLOCK_TASK_ITEM || cur->list_cont;
    if (prev_list && !cur_list) return t->unit * 0.55f;   /* list closes */
    if (!prev_list && cur_list && prev->kind != MD_BLOCK_HEADING)
        return t->unit * 0.30f;                           /* list opens */
    return 0.0f;
}

f32 md_render(MdDoc *doc, MdRenderCtx *ctx) {
    if (!doc || !ctx || !ctx->r || !ctx->theme ||
        (doc->block_count && !doc->blocks) ||
        !isfinite(ctx->x) || !isfinite(ctx->y) || !isfinite(ctx->w) ||
        !isfinite(ctx->h) || !isfinite(ctx->cw) || !isfinite(ctx->ch) ||
        !isfinite(ctx->dpi) || !isfinite(ctx->scroll_y) ||
        ctx->w <= 0.0f || ctx->h <= 0.0f || ctx->cw <= 0.0f || ctx->ch <= 0.0f ||
        ctx->cw > 16384.0f || ctx->ch > 16384.0f || ctx->dpi <= 0.0f ||
        !isfinite(ctx->x + ctx->w) || !isfinite(ctx->y + ctx->h))
        return 0.0f;

    /* Reset the link-rect collector at the top of each render — the buffer
     * is owned by the caller (typically reused across frames). */
    if (ctx->link_rects && ctx->link_rect_count) *ctx->link_rect_count = 0;
    if (ctx->glyph_rects && ctx->glyph_rect_count) *ctx->glyph_rect_count = 0;
    if (ctx->outline && ctx->outline_count) *ctx->outline_count = 0;
    if (ctx->task_rects && ctx->task_rect_count) *ctx->task_rect_count = 0;
    if (ctx->fold_rects && ctx->fold_rect_count) *ctx->fold_rect_count = 0;

    /* Open a new image-cache pass. Everything drawn from here on is pinned
     * against budget eviction until the next md_render, which stops a visible
     * set larger than the cache budget from evicting its own members and
     * re-decoding them on every frame. */
    md_image_cache_begin_pass(ctx->images);
    md_embed_cache_begin_pass(ctx->embeds);
    u64 image_revision = ctx->images ? ctx->images->layout_revision : 0;
    u64 embed_revision = md_embed_cache_revision(ctx->embeds);
    if (doc->image_revision != image_revision || doc->embed_revision != embed_revision) {
        doc->image_revision = image_revision; doc->embed_revision = embed_revision;
        doc->layout_w = -1.0f;
    }

    /* Pre-compute the per-frame palette + every text/surface color the
     * block renderers consume. Done once here so ir_make and render_code
     * stay cheap, rather than running chrome_palette_for and the
     * bisecting WCAG contrast checks per block (and per cell, in
     * tables). */
    {
        const Theme *t = ctx->theme;
        ChromePalette cp = chrome_palette_for(t);
        ctx->_is_light       = cp.is_light;
        ctx->_surface_raised = cp.surface_raised;
        ctx->_fg             = t->fg;
        ctx->_fg_dim         = color_dim(t->fg, 0.75f);
        ctx->_fg_bright      = color_emphasize(t->fg, t->bg, 0.25f);
        ctx->_link           = color_ensure_contrast(t->ansi[12], t->bg, 4.0f);
        ctx->_code_bg        = cp.surface_raised;
        ctx->_code_bg.a      = cp.is_light ? 0.85f : 0.55f;
        ctx->_code_fg        = color_ensure_contrast(t->ansi[3], cp.surface_raised, 4.5f);
        ctx->_tag_fg         = color_ensure_contrast(t->ansi[13], t->bg, 4.0f);
        ctx->_tag_bg         = color_with_alpha(t->ansi[5], cp.is_light ? 0.22f : 0.18f);
        /* Use the high-contrast body color for headings and reserve the accent
         * for links. Heading hierarchy comes from size and spacing. */
        ctx->_heading_bright = color_emphasize(t->fg, t->bg, 0.35f);
        ctx->_heading_fg     = color_emphasize(t->fg, t->bg, 0.18f);
        ctx->_muted          = color_with_alpha(t->fg, 0.58f);
        ctx->_rule           = color_with_alpha(
                                 color_ensure_contrast(t->border, t->bg, 1.6f),
                                 cp.is_light ? 0.55f : 0.45f);
        ctx->_marker         = color_with_alpha(ctx->_link, 0.85f);
        ctx->_quote_bg       = color_with_alpha(cp.surface_raised,
                                                cp.is_light ? 0.7f : 0.40f);
        ctx->_quote_bar      = color_with_alpha(ctx->_link, 0.55f);
        Color panel_bg       = cp.surface_raised; panel_bg.a = 1.0f;
        ctx->_code_block_panel_bg = panel_bg;
        /* Default body text reads against the panel; the syntax palette below
         * layers semantic colors on top of it for blocks that name a language. */
        ctx->_code_block_text_fg  = color_ensure_contrast(t->fg, panel_bg, 4.5f);
        ctx->_code_kw  = color_ensure_contrast(t->ansi[12], panel_bg, 3.5f);  /* blue   */
        ctx->_code_str = color_ensure_contrast(t->ansi[3],  panel_bg, 3.5f);  /* yellow */
        ctx->_code_cmt = color_with_alpha(
                          color_ensure_contrast(t->ansi[2], panel_bg, 2.5f), 0.75f); /* dim green */
        ctx->_code_num = color_ensure_contrast(t->ansi[10], panel_bg, 3.5f);  /* bright green */
        ctx->_code_pre = color_ensure_contrast(t->ansi[14], panel_bg, 3.5f);  /* cyan */
        ctx->_code_typ = color_ensure_contrast(t->ansi[14], panel_bg, 3.5f);  /* cyan */
        ctx->_code_fn  = color_ensure_contrast(t->ansi[12], panel_bg, 3.5f);  /* blue */
        /* A ```diff fence is the one block where green and red carry the
         * meaning every reader already knows; the scanner only ever emits
         * these two kinds for SYN_LANG_DIFF. */
        ctx->_code_ins = color_ensure_contrast(t->ansi[10], panel_bg, 3.5f);  /* bright green */
        ctx->_code_del = color_ensure_contrast(t->ansi[9],  panel_bg, 3.5f);  /* bright red */
        ctx->_derived_ready  = true;
    }

    /* Typographic scale for this frame. Everything below measures and places
     * against this — never against the raw (cw, ch) the caller handed in. */
    ctx->_t = md_typo_make(ctx->r, ctx->cw, ctx->ch, ctx->dpi);

    /* Measure column. Past ~90 characters the eye loses the start of the next
     * line, so a wide viewport gets margins rather than longer lines; the
     * column is centred so the page reads as a document instead of as text
     * pinned to the left edge. Everything downstream is laid out from
     * (col_x, col_w), and the collectors record absolute rects, so hit-testing
     * and selection follow the column automatically. */
    f32 col_w = fminf(ctx->w, MD_MEASURE_COLS * ctx->_t.cw);
    if (col_w < ctx->_t.cw * 8.0f) col_w = ctx->w;
    f32 col_x = ctx->x + roundf((ctx->w - col_w) * 0.5f);

    f32 max_x = col_x + col_w;
    f32 y_top = ctx->y;
    f32 y_bottom = ctx->y + ctx->h;
    f32 scroll = ctx->scroll_y < 0.0f ? 0.0f : ctx->scroll_y;

    /* Body line box: the renderer centres each glyph inside it, which is what
     * turns the leading computed above into real air between lines. Restored
     * before returning so the caller's chrome (title bar, scrollbar) keeps
     * drawing on its own metrics. */
    renderer_set_ui_scale(ctx->r, ctx->_t.cw, ctx->_t.lh);

    /* Virtual cursor walks the entire document; scroll offset is subtracted
     * so on-screen y = virtual_y - scroll. Blocks whose computed band sits
     * outside [y_top, y_bottom) are walked in measure-only mode, so their
     * height still counts toward the total content_px return value without
     * paying the per-glyph push cost or burning batched quads off-screen; a
     * frame emits glyphs only for the few blocks on screen. */
    /* Bump the layout generation when the wrap width or cell metrics change;
     * any block height cached under the previous layout is then stale. */
    if (doc->layout_w != col_w || doc->layout_cw != ctx->cw ||
        doc->layout_ch != ctx->ch) {
        doc->layout_w  = col_w;
        doc->layout_cw = ctx->cw;
        doc->layout_ch = ctx->ch;
        doc->layout_gen++;
        if (doc->layout_gen == 0) doc->layout_gen = 1;  /* 0 == "never cached" */
    }
    ctx->_layout_gen = doc->layout_gen;

    /* Walk the whole document; on-screen blocks emit glyphs, off-screen blocks
     * only need their height to advance the virtual cursor. Block heights are
     * invariant for a fixed (w, cw, ch), so a block that's off-screen with a
     * height already cached for this layout skips the per-codepoint measure
     * walk entirely — steady-state scroll becomes O(blocks), not O(glyphs). */
    f32 virt_y = 0.0f;
    bool save_measure = ctx->measure_only;
    for (u32 i = 0; i < doc->block_count; i++) {
        MdBlock *blk = &doc->blocks[i];
        /* Applied to the cursor, not folded into either block's cached height,
         * so the cache stays keyed on the block alone. */
        virt_y += block_lead(i ? &doc->blocks[i - 1] : NULL, blk, &ctx->_t);
        /* Outline capture: every heading, regardless of on/off-screen, so the
         * collected table is the full document table-of-contents. doc_y is the
         * heading's pre-scroll top. */
        /* Anchor seek: hand back this block's document-space top so the caller
         * can scroll a freshly opened [[Note#Heading]] straight to it. */
        if (ctx->seek_block_y && i == ctx->seek_block) *ctx->seek_block_y = virt_y;
        if (blk->kind == MD_BLOCK_HEADING && ctx->outline && ctx->outline_count &&
            *ctx->outline_count < ctx->outline_cap) {
            MdSlice lbl = heading_label_slice(blk);
            ctx->outline[*ctx->outline_count] = (MdOutlineItem){
                .doc_y = virt_y, .level = blk->heading_level,
                .text = lbl.data, .text_len = lbl.len,
            };
            (*ctx->outline_count)++;
        }
        f32 effective_y = y_top + (virt_y - scroll);
        /* Height used for the visibility test. It has to be the BLOCK's own
         * height: with the viewport height standing in for it, any block
         * taller than the viewport would be declared off-screen the moment
         * its top scrolled one screenful past, blanking a long code block or
         * a big table while its middle is still being read. A block not yet
         * measured under this layout has no height to use, so it is assumed
         * visible; that measures it, and every later frame has the real
         * number. */
        if (blk->cached_gen != doc->layout_gen && effective_y < y_top) {
            bool exact = ctx->measure_exact;
            ctx->measure_only = true;
            ctx->measure_exact = true;
            f32 bottom = render_block(blk, ctx, col_x, effective_y, max_x);
            blk->cached_h = fmaxf(ctx->_t.lh, bottom - effective_y);
            blk->cached_gen = doc->layout_gen;
            ctx->measure_exact = exact;
            ctx->measure_only = save_measure;
        }
        f32 known_h = (blk->cached_gen == doc->layout_gen) ? blk->cached_h : ctx->h;
        if (known_h < ctx->_t.lh) known_h = ctx->_t.lh;
        bool maybe_visible = (effective_y < y_bottom) &&
                             (blk->cached_gen != doc->layout_gen || effective_y + known_h > y_top);
        bool need_draw = !save_measure && (maybe_visible || ctx->collect_all);
        f32 block_h;
        if (!need_draw && blk->cached_gen == doc->layout_gen) {
            block_h = blk->cached_h;   /* cached: no measure walk, no emit */
        } else {
            ctx->measure_only = !need_draw;
            u32 first_glyph = glyph_list_len(ctx);
            f32 next_y = render_block(blk, ctx, col_x, effective_y, max_x);
            block_mark_text(ctx, first_glyph, i ? blk - 1 : NULL, blk);
            block_h = next_y - effective_y;
            if (!isfinite(block_h) || block_h < 0.0f) block_h = ctx->_t.lh;
            blk->cached_h   = block_h;
            blk->cached_gen = doc->layout_gen;
        }
        virt_y += block_h;
    }
    ctx->measure_only = save_measure;
    renderer_flush_rects(ctx->r);
    renderer_flush_glyphs(ctx->r);
    renderer_set_ui_scale(ctx->r, ctx->cw, ctx->ch);
    /* Trailing air so the last line of a document doesn't sit flush against
     * the bottom edge of the viewport. */
    return virt_y + ctx->_t.unit;
}

/* The text of a glyph list — see md_render.h. The rects say what is on which
 * row; the flags say which of the row changes are the document's own.
 *
 * Between two glyphs, the first rule that applies:
 *   BLOCK        a blank line (two breaks)
 *   CELL         a tab, whatever the rows say — a cell that wrapped ends lower
 *                than the next cell of its row begins
 *   another row  a line break, unless the row only WRAPs the one above
 *   GAP          a space (also across a wrap: the gap is the text's, the row
 *                is the window's)
 * Nothing is written before the first glyph of the range: a selection that
 * starts at a block's first letter does not start with a blank line. */
usize md_glyphs_text(const MdGlyphRect *rects, u32 lo, u32 hi, char *out, usize cap) {
    usize n = 0;
    bool  any = false;         /* something of the range has been read */
    f32   row_y = 0.0f;
    u8    carried = 0;         /* the BLOCK flag of a glyph that was skipped */
    for (u32 i = lo; rects && i < hi; i++) {
        const MdGlyphRect *g = &rects[i];
        u8 flags = (u8)(g->flags | carried);
        if (g->flags & MD_GLYPH_CHROME) {
            carried = (u8)(flags & MD_GLYPH_BLOCK);
            continue;
        }
        carried = 0;
        u8  enc[UTF8_CLUSTER_MAX * 4 + 2];
        u32 el = 0;
        if (any) {
            bool new_row = fabsf(g->y - row_y) >= 0.5f;
            if (flags & MD_GLYPH_BLOCK)       { enc[el++] = '\n'; enc[el++] = '\n'; }
            else if (flags & MD_GLYPH_CELL)   { enc[el++] = '\t'; }
            else if (new_row && !(flags & MD_GLYPH_WRAP)) { enc[el++] = '\n'; }
            else if (flags & MD_GLYPH_GAP)    { enc[el++] = ' '; }
        }
        any   = true;
        row_y = g->y;
        /* A glyph's cp can be a cluster HANDLE (the reader walks text with
         * utf8_next_grapheme): plain utf8_encode returns 0 for one, and every
         * flag, ZWJ family and VS16 emoji dropped out of the copy. */
        if (g->cp) el += utf8_cluster_encode(g->cp, enc + el, (u32)sizeof enc - el);
        for (u32 k = 0; k < el; k++, n++)
            if (out && n + 1 < cap) out[n] = (char)enc[k];
    }
    if (out && cap) out[n < cap ? n : cap - 1] = '\0';
    return n;
}
