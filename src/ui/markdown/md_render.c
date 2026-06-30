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
#include "ui/chrome_palette.h"
#include "core/utf8.h"

#include <math.h>
#include <string.h>
#include <stdio.h>

/* =========================================================================
 * Static palette
 * ========================================================================= */

static const f32 H_SCALE[7] = { 1.0f, 1.6f, 1.4f, 1.2f, 1.1f, 1.0f, 0.95f };

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
 * backgrounds we lighten; on light backgrounds we darken. The legacy code
 * blindly lightened toward white, which IS the bg on light themes — so
 * bold text disappeared (Sakura). */
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
     * collapsed onto the anchor. Cheap (≤4 steps) and we don't care about
     * the optimum, just "good enough". */
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
    f32 cw, ch;
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
    u8          link_kind;        /* MdLinkRectKind */
    f32         link_seg_start_cx;
    f32         link_seg_start_line_y;

    /* Optional per-glyph rect collector for text selection (mirrors the
     * MdRenderCtx fields; wired in ir_make on the visible pass only). */
    MdGlyphRect *glyph_buf;
    u32          glyph_cap;
    u32         *glyph_count;

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
    if (!ir->link_active || !ir->link_buf || !ir->link_count) return;
    if (ir->cx <= ir->link_seg_start_cx) return;
    if (*ir->link_count >= ir->link_cap) return;
    ir->link_buf[*ir->link_count] = (MdLinkRect){
        .x       = ir->link_seg_start_cx,
        .y       = ir->link_seg_start_line_y,
        .w       = ir->cx - ir->link_seg_start_cx,
        .h       = ir->ch,
        .url     = ir->link_url,
        .url_len = ir->link_url_len,
        .kind    = ir->link_kind,
    };
    (*ir->link_count)++;
}

static inline void ir_link_begin(InlineRender *ir, MdSlice url, u8 kind) {
    if (!ir->link_buf || !ir->link_count || url.len == 0) {
        ir->link_active = false;
        return;
    }
    ir->link_active           = true;
    ir->link_url              = url.data;
    ir->link_url_len          = url.len;
    ir->link_kind             = kind;
    ir->link_seg_start_cx     = ir->cx;
    ir->link_seg_start_line_y = ir->line_y;
}

static inline void ir_link_end(InlineRender *ir) {
    if (!ir->link_active) return;
    ir_link_flush_segment(ir);
    ir->link_active = false;
}

/* Local emit wrappers that respect measure_only.
 *
 * Title-bar bleed when scrolled is handled by drawing the title bar AFTER
 * markdown render in fb_render_viewer (it overdraws any partial top-line).
 * We don't y-clip individual glyphs/rects here because partial-overlap
 * tolerances would cause whole top-edge glyphs to blink in/out during
 * smooth scroll — a worse artefact than a one-pixel bleed. */
static inline void mr_push_glyph(InlineRender *ir, f32 x, f32 y, u32 cp, Color fg) {
    if (ir->measure_only) return;
    renderer_push_glyph(ir->r, x, y, cp, fg);
    if (ir->glyph_buf && ir->glyph_count && *ir->glyph_count < ir->glyph_cap) {
        ir->glyph_buf[*ir->glyph_count] =
            (MdGlyphRect){ x, y, ir->cw, ir->ch, cp, ir->cur_src_off };
        (*ir->glyph_count)++;
    }
}
static inline void mr_draw_rect(InlineRender *ir, f32 x, f32 y, f32 w, f32 h, Color c) {
    if (ir->measure_only) return;
    renderer_draw_rect(ir->r, x, y, w, h, c);
}

static inline void cr_push_glyph(const MdRenderCtx *ctx, f32 x, f32 y, u32 cp, Color fg) {
    if (ctx->measure_only) return;
    renderer_push_glyph(ctx->r, x, y, cp, fg);
    if (ctx->glyph_rects && ctx->glyph_rect_count &&
        *ctx->glyph_rect_count < ctx->glyph_rect_cap) {
        ctx->glyph_rects[*ctx->glyph_rect_count] =
            (MdGlyphRect){ x, y, ctx->cw, ctx->ch, cp, MD_GLYPH_NO_SRC };
        (*ctx->glyph_rect_count)++;
    }
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
}

/* Walk a slice of UTF-8 bytes, emitting codepoints with the supplied color.
 * Word-aware soft wrap: at each word boundary we look ahead and, if the whole
 * word would overflow the current line, move it down first so the break lands
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
    bool s_in_src = ir->src_base && s.data >= ir->src_base &&
                    s.data + s.len <= ir->src_base + ir->src_len;
    u32 s_base_off = s_in_src ? (u32)(s.data - ir->src_base) : MD_GLYPH_NO_SRC;
    u32 i = 0;
    while (i < s.len) {
        u32 cp = 0;
        u32 n = utf8_decode(s.data + i, s.len - i, &cp);
        if (n == 0) { i++; continue; }
        ir->cur_src_off = s_in_src ? s_base_off + i : MD_GLYPH_NO_SRC;

        if (cp == '\n') {
            ir_newline(ir);
            i += n;
            at_word_start = true;
            continue;
        }

        if (cp == ' ' || cp == '\t') {
            /* Break at the space if drawing it would overflow; otherwise
             * advance past it (underline/strike continue across it). */
            if (ir->cx + step > ir->max_x) {
                ir_newline(ir);
            } else {
                if (cp >= 32) mr_push_glyph(ir, ir->cx, ir->line_y, cp, fg);
                if (with_underline)
                    mr_draw_rect(ir, ir->cx, ir->line_y + ir->ch - 1.0f,
                                 step, fmaxf(1.0f, ir->ch * 0.05f), fg);
                if (with_strike)
                    mr_draw_rect(ir, ir->cx, ir->line_y + ir->ch * 0.5f,
                                 step, fmaxf(1.0f, ir->ch * 0.06f), fg);
                ir->cx += step;
            }
            i += n;
            at_word_start = true;
            continue;
        }

        /* Word character. At the start of a word, look ahead: if the whole
         * word would overflow the current line (and we're not already at the
         * line start), move it down so we break between words, not mid-word. */
        if (at_word_start) {
            u32 word_cps = 0;
            for (u32 j = i; j < s.len; ) {
                u32 wcp = 0;
                u32 wn = utf8_decode(s.data + j, s.len - j, &wcp);
                if (wn == 0) { j++; continue; }
                if (wcp == ' ' || wcp == '\t' || wcp == '\n') break;
                word_cps++;
                j += wn;
            }
            if (ir->cx > ir->x_left &&
                ir->cx + (f32)word_cps * step > ir->max_x) {
                ir_newline(ir);
            }
            at_word_start = false;
        }

        /* Mid-word hard break: only for a word wider than the whole line. */
        if (ir->cx + step > ir->max_x && ir->cx > ir->x_left) {
            ir_newline(ir);
        }

        if (cp >= 32) mr_push_glyph(ir, ir->cx, ir->line_y, cp, fg);
        if (with_underline)
            mr_draw_rect(ir, ir->cx, ir->line_y + ir->ch - 1.0f,
                         step, fmaxf(1.0f, ir->ch * 0.05f), fg);
        if (with_strike)
            mr_draw_rect(ir, ir->cx, ir->line_y + ir->ch * 0.5f,
                         step, fmaxf(1.0f, ir->ch * 0.06f), fg);
        ir->cx += step;
        i += n;
    }
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
    usize cells = utf8_len(s.data, s.len);
    f32 span = (f32)cells * scw;
    if (ir->cx + span > ir->max_x && ir->cx > ir->x_left) ir_newline(ir);
    if (!ir->measure_only && cells > 0) {
        renderer_flush_glyphs(ir->r);
        renderer_set_ui_scale(ir->r, ir->cw * sc, ir->ch * sc);
        f32 ytop = sup ? ir->line_y : ir->line_y + (ir->ch - ir->ch * sc);
        f32 cx = ir->cx;
        u32 i = 0;
        while (i < s.len) {
            u32 cp = 0; u32 nn = utf8_decode(s.data + i, s.len - i, &cp);
            if (!nn) { i++; continue; }
            if (cp >= 32) { renderer_push_glyph(ir->r, cx, ytop, cp, ir->fg); cx += scw; }
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
            if (r->child_count)
                ir_emit_runs(ir, r->children, r->child_count, ir->fg_bright, underline, strike);
            else
                ir_emit_bytes(ir, r->text, ir->fg_bright, underline, strike);
            break;
        case MD_INLINE_ITALIC:
            if (r->child_count)
                ir_emit_runs(ir, r->children, r->child_count, ir->fg_dim, underline, strike);
            else
                ir_emit_bytes(ir, r->text, ir->fg_dim, underline, strike);
            break;
        case MD_INLINE_STRIKETHROUGH:
            if (r->child_count)
                ir_emit_runs(ir, r->children, r->child_count, ir->fg_dim, underline, true);
            else
                ir_emit_bytes(ir, r->text, ir->fg_dim, underline, true);
            break;
        case MD_INLINE_HIGHLIGHT: {
            usize w_cells = utf8_len(r->text.data, r->text.len);
            f32 span = (f32)w_cells * ir->cw;
            if (ir->cx + span > ir->max_x) ir_newline(ir);
            mr_draw_rect(ir, ir->cx - 1, ir->line_y, span + 2, ir->ch, ir->hl_bg);
            if (r->child_count)
                ir_emit_runs(ir, r->children, r->child_count, base_fg, underline, strike);
            else
                ir_emit_bytes(ir, r->text, base_fg, underline, strike);
            break;
        }
        case MD_INLINE_CODE: {
            /* width = codepoint count * cw (rough but close enough) */
            usize bytes = r->text.len;
            usize w_cells = utf8_len(r->text.data, bytes);
            f32 span = (f32)w_cells * ir->cw;
            if (ir->cx + span > ir->max_x) ir_newline(ir);
            mr_draw_rect(ir, ir->cx - 1, ir->line_y - 1,
                               span + 2, ir->ch + 2, ir->code_bg);
            ir_emit_bytes(ir, r->text, ir->code_fg, false, false);
            break;
        }
        case MD_INLINE_LINK:
        case MD_INLINE_AUTOLINK:
        case MD_INLINE_WIKILINK: {
            MdSlice display = r->text.len ? r->text : r->url;
            if (r->kind == MD_INLINE_WIKILINK && r->alt.len) display = r->alt;
            else if (r->kind == MD_INLINE_WIKILINK)          display = r->url;
            /* AUTOLINK is parsed via push_styled_run which stores the URL
             * in `text`, not `url` — both display and click target are the
             * same slice. WIKILINK is "inert in v1" per md_ast.h. */
            MdSlice click_url = (r->kind == MD_INLINE_AUTOLINK) ? r->text : r->url;
            /* Wikilinks are now clickable too (resolve to a .md on click). */
            u8 lk = (r->kind == MD_INLINE_WIKILINK) ? MD_LINKRECT_WIKILINK
                                                    : MD_LINKRECT_URL;
            ir_link_begin(ir, click_url, lk);
            if (r->kind == MD_INLINE_LINK && r->child_count)
                ir_emit_runs(ir, r->children, r->child_count, ir->link, true, strike);
            else
                ir_emit_bytes(ir, display, ir->link, true, false);
            ir_link_end(ir);
            break;
        }
        case MD_INLINE_TAG: {
            usize w_cells = utf8_len(r->text.data, r->text.len) + 1;
            f32 span = (f32)w_cells * ir->cw;
            if (ir->cx + span > ir->max_x) ir_newline(ir);
            mr_draw_rect(ir, ir->cx - 2, ir->line_y - 1,
                               span + 4, ir->ch + 2, ir->tag_bg);
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
            if (mlabel) {
                /* basename of the target */
                const u8 *nm = tgt.data; u32 nl = tgt.len;
                for (i32 k = (i32)tgt.len - 1; k >= 0; k--)
                    if (tgt.data[k] == '/') { nm = tgt.data + k + 1; nl = tgt.len - (u32)(k + 1); break; }
                u32 lbl = (u32)strlen(mlabel);
                /* Cap the name so the chip fits one line (the pill background is
                 * a single fixed-width rect; a wrapped name would escape it). */
                u32 line_cells = (u32)((ir->max_x - ir->x_left) / ir->cw);
                if (line_cells < lbl + 4) line_cells = lbl + 4;
                u32 max_name = line_cells - lbl - 2;
                if (nl > max_name) nl = max_name;
                f32 span = (f32)(lbl + 1 + nl) * ir->cw;
                if (ir->cx + span > ir->max_x && ir->cx > ir->x_left) ir_newline(ir);
                mr_draw_rect(ir, ir->cx - 2, ir->line_y - 1, span + 4, ir->ch + 2, ir->tag_bg);
                ir_link_begin(ir, tgt, MD_LINKRECT_EMBED);
                MdSlice ls = { (const u8 *)mlabel, lbl };
                ir_emit_bytes(ir, ls, ir->tag_fg, false, false);
                ir->cx += ir->cw;   /* gap */
                MdSlice ns = { nm, nl };
                ir_emit_bytes(ir, ns, ir->fg, false, false);
                ir_link_end(ir);
            } else {
                ir_link_begin(ir, r->url, r->kind == MD_INLINE_IMAGE
                                              ? MD_LINKRECT_IMAGE : MD_LINKRECT_EMBED);
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
            if (ir->cx + mw > ir->max_x && ir->cx > ir->x_left) ir_newline(ir);
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

static InlineRender ir_make(MdRenderCtx *ctx, f32 line_y, f32 x, f32 max_x,
                            f32 cw, f32 ch) {
    InlineRender ir = {0};
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
    return b->inline_count >= 2 &&
           b->inlines[0].kind == MD_INLINE_FOOTNOTE_REF &&
           b->inlines[1].kind == MD_INLINE_TEXT &&
           b->inlines[1].text.len >= 1 && b->inlines[1].text.data[0] == ':';
}

/* Render a footnote definition: dim body with a thin accent bar in the gutter,
 * visually setting the back-matter apart from the document body. */
static f32 render_footnote_def(const MdBlock *b, MdRenderCtx *ctx,
                               f32 x, f32 y, f32 max_x) {
    f32 ch = ctx->ch;
    f32 bar_w = fmaxf(1.5f, ctx->dpi * 1.0f);
    Color bar = color_with_alpha(ctx->theme->ansi[12], 0.5f);
    f32 start_y = y;
    InlineRender ir = ir_make(ctx, y, x + bar_w + 8, max_x, ctx->cw, ch);
    ir.fg = color_dim(ctx->theme->fg, 0.72f);
    ir_emit_inlines(&ir, b->inlines, b->inline_count);
    f32 end_y = ir_finish(&ir);
    cr_draw_rect(ctx, x, start_y, bar_w, end_y - start_y, bar);
    return end_y + ch * 0.4f;
}

static f32 render_paragraph(const MdBlock *b, MdRenderCtx *ctx,
                             f32 x, f32 y, f32 max_x) {
    if (!ctx->in_nested_block && para_is_footnote_def(b))
        return render_footnote_def(b, ctx, x, y, max_x);
    InlineRender ir = ir_make(ctx, y, x, max_x, ctx->cw, ctx->ch);
    ir_emit_inlines(&ir, b->inlines, b->inline_count);
    return ir_finish(&ir) + ctx->ch * 0.4f;
}

/* Definition-list term — rendered bright/bold at the left margin. */
static f32 render_def_term(const MdBlock *b, MdRenderCtx *ctx,
                           f32 x, f32 y, f32 max_x) {
    InlineRender ir = ir_make(ctx, y, x, max_x, ctx->cw, ctx->ch);
    ir.fg = ctx->_fg_bright;
    ir_emit_runs(&ir, b->inlines, b->inline_count, ctx->_fg_bright, false, false);
    return ir_finish(&ir);
}

/* Definition-list description — indented under its term. */
static f32 render_def_desc(const MdBlock *b, MdRenderCtx *ctx,
                           f32 x, f32 y, f32 max_x) {
    f32 indent = 2.0f * ctx->cw;
    InlineRender ir = ir_make(ctx, y, x + indent, max_x, ctx->cw, ctx->ch);
    ir_emit_inlines(&ir, b->inlines, b->inline_count);
    return ir_finish(&ir) + ctx->ch * 0.25f;
}

static f32 render_heading(const MdBlock *b, MdRenderCtx *ctx,
                           f32 x, f32 y, f32 max_x) {
    f32 scale = H_SCALE[b->heading_level <= 6 ? b->heading_level : 6];
    /* Integer cell advance so every heading glyph steps by a constant width
     * (renderer_push_glyph floors x; a fractional advance produces 25/26px
     * tracking jitter). */
    f32 hcw = roundf(ctx->cw * scale);
    f32 hch = ctx->ch * scale;
    f32 top_pad = b->heading_level <= 2 ? hch * 0.5f : hch * 0.25f;
    y += top_pad;
    if (!ctx->measure_only) renderer_set_ui_scale(ctx->r, hcw, hch);
    InlineRender ir = ir_make(ctx, y, x, max_x, hcw, hch);
    ir.fg = ctx->_heading_fg;
    ir.fg_bright = ctx->_heading_bright;
    ir_emit_inlines(&ir, b->inlines, b->inline_count);
    f32 end_y = ir_finish(&ir);
    /* Underline H1/H2 */
    if (b->heading_level <= 2) {
        cr_draw_rect(ctx, x, end_y + 1.0f, max_x - x,
                           fmaxf(1.0f, ctx->dpi * 0.5f),
                           color_with_alpha(ctx->theme->border, 0.7f));
        end_y += hch * 0.25f;
    }
    /* Restore the viewer's UI scale — renderer_reset_ui_scale would drop
     * back to the atlas's natural metrics (~25x49 px) instead of the 8x16
     * pt the file-browser pre-configured, leaving every following block
     * with mismatched glyph height vs. line advance and overlapping text. */
    if (!ctx->measure_only) renderer_set_ui_scale(ctx->r, ctx->cw, ctx->ch);
    return end_y + hch * 0.4f;
}

/* Tab stop for fenced code body. CommonMark says "tabs are preserved" — for
 * display we expand to the next multiple of TAB_WIDTH so indented code reads
 * naturally instead of collapsing to single cells. */
#define MD_CODE_TAB_WIDTH 4u

/* =========================================================================
 * Code-block syntax highlighter
 *
 * Self-contained per-block lexer covering the handful of languages our
 * fixtures + docs use most. The goal is "good enough to skim" — keywords,
 * strings, comments and numbers. Not a real parser. State persists across
 * lines so multi-line block comments and Python triple-quoted strings
 * stay coloured correctly.
 * ========================================================================= */

typedef enum {
    CODE_LANG_NONE = 0,
    CODE_LANG_C,
    CODE_LANG_PYTHON,
    CODE_LANG_RUST,
    CODE_LANG_SHELL,
    CODE_LANG_JS,
    CODE_LANG_GO,
    CODE_LANG_JSON,
} CodeLang;

typedef enum {
    CTK_DEFAULT = 0,
    CTK_KEYWORD,
    CTK_STRING,
    CTK_COMMENT,
    CTK_NUMBER,
    CTK_PREPROC,
} CodeTokKind;

typedef struct {
    CodeLang lang;
    bool     in_block_comment;   /* inside a C-style slash-star comment */
    u8       triple_quote;       /* 0 / 0x22 / 0x27 — Python triple-quoted string */
} CodeLexState;

static const char *const KW_C[] = {
    "auto","break","case","char","const","continue","default","do","double","else","enum",
    "extern","float","for","goto","if","inline","int","long","register","restrict","return",
    "short","signed","sizeof","static","struct","switch","typedef","union","unsigned",
    "void","volatile","while","_Bool","bool","true","false","NULL","nullptr",
    "class","public","private","protected","virtual","new","delete","this","template",
    "namespace","using","throw","try","catch","operator","friend", NULL
};
static const char *const KW_PY[] = {
    "False","None","True","and","as","assert","async","await","break","class","continue","def",
    "del","elif","else","except","finally","for","from","global","if","import","in","is","lambda",
    "nonlocal","not","or","pass","raise","return","try","while","with","yield","match","case", NULL
};
static const char *const KW_RUST[] = {
    "as","async","await","break","const","continue","crate","dyn","else","enum","extern","false",
    "fn","for","if","impl","in","let","loop","match","mod","move","mut","pub","ref","return",
    "self","Self","static","struct","super","trait","true","type","unsafe","use","where","while",
    "yield","union","box", NULL
};
static const char *const KW_SH[] = {
    "if","then","else","elif","fi","case","esac","for","while","until","do","done","function",
    "return","in","local","export","unset","alias","echo","read","cd","pwd","source","exit", NULL
};
static const char *const KW_JS[] = {
    "abstract","arguments","await","boolean","break","byte","case","catch","char","class","const",
    "continue","debugger","default","delete","do","double","else","enum","eval","export","extends",
    "false","final","finally","float","for","function","goto","if","implements","import","in",
    "instanceof","int","interface","let","long","native","new","null","package","private",
    "protected","public","return","short","static","super","switch","synchronized","this","throw",
    "throws","transient","true","try","typeof","var","void","volatile","while","with","yield",
    "async","of", NULL
};
static const char *const KW_GO[] = {
    "break","case","chan","const","continue","default","defer","else","fallthrough","for","func",
    "go","goto","if","import","interface","map","package","range","return","select","struct",
    "switch","type","var","true","false","nil","iota", NULL
};
static const char *const KW_JSON[] = { "true","false","null", NULL };

static CodeLang code_lang_from_info(const u8 *data, u32 len) {
    if (!data || len == 0) return CODE_LANG_NONE;
    char buf[16] = {0};
    u32 n = len < (u32)sizeof(buf) - 1 ? len : (u32)sizeof(buf) - 1;
    for (u32 i = 0; i < n; i++) {
        u8 c = data[i];
        buf[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : (char)c;
    }
    if (!strcmp(buf,"c")||!strcmp(buf,"h")||!strcmp(buf,"cpp")||!strcmp(buf,"cxx")||
        !strcmp(buf,"cc")||!strcmp(buf,"c++")||!strcmp(buf,"hpp")||
        !strcmp(buf,"objc")||!strcmp(buf,"objcpp")||!strcmp(buf,"m")||!strcmp(buf,"mm"))
        return CODE_LANG_C;
    if (!strcmp(buf,"python")||!strcmp(buf,"py"))                  return CODE_LANG_PYTHON;
    if (!strcmp(buf,"rust")||!strcmp(buf,"rs"))                    return CODE_LANG_RUST;
    if (!strcmp(buf,"sh")||!strcmp(buf,"bash")||!strcmp(buf,"shell")||!strcmp(buf,"zsh"))
        return CODE_LANG_SHELL;
    if (!strcmp(buf,"js")||!strcmp(buf,"javascript")||!strcmp(buf,"ts")||!strcmp(buf,"typescript")||
        !strcmp(buf,"jsx")||!strcmp(buf,"tsx"))
        return CODE_LANG_JS;
    if (!strcmp(buf,"go")||!strcmp(buf,"golang"))                  return CODE_LANG_GO;
    if (!strcmp(buf,"json"))                                       return CODE_LANG_JSON;
    return CODE_LANG_NONE;
}

static bool code_kw_match(const char *const *list, const u8 *w, u32 wlen) {
    for (u32 i = 0; list[i]; i++) {
        if (strlen(list[i]) == wlen && memcmp(list[i], w, wlen) == 0) return true;
    }
    return false;
}
static bool code_is_keyword(CodeLang lang, const u8 *w, u32 wlen) {
    switch (lang) {
    case CODE_LANG_C:      return code_kw_match(KW_C, w, wlen);
    case CODE_LANG_PYTHON: return code_kw_match(KW_PY, w, wlen);
    case CODE_LANG_RUST:   return code_kw_match(KW_RUST, w, wlen);
    case CODE_LANG_SHELL:  return code_kw_match(KW_SH, w, wlen);
    case CODE_LANG_JS:     return code_kw_match(KW_JS, w, wlen);
    case CODE_LANG_GO:     return code_kw_match(KW_GO, w, wlen);
    case CODE_LANG_JSON:   return code_kw_match(KW_JSON, w, wlen);
    default: return false;
    }
}

static inline bool code_ident_start(u8 c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
static inline bool code_ident_cont(u8 c) {
    return code_ident_start(c) || (c >= '0' && c <= '9');
}

/* Consume one token starting at *pi, advance *pi past the token, return the
 * token kind. `len` is the byte cap (e.g. line end or block end) — the lexer
 * never reads past it. Mid-token state (block comments, triple strings) is
 * carried through `st` so a token can naturally span multiple lines when the
 * caller iterates line-by-line. */
static CodeTokKind code_eat_token(CodeLexState *st, const u8 *src, u32 len, u32 *pi) {
    u32 i = *pi;
    if (i >= len) { *pi = len; return CTK_DEFAULT; }

    if (st->in_block_comment) {
        while (i < len) {
            if (i + 1 < len && src[i] == '*' && src[i+1] == '/') {
                i += 2; st->in_block_comment = false; break;
            }
            i++;
        }
        *pi = i;
        return CTK_COMMENT;
    }
    if (st->triple_quote) {
        u8 q = st->triple_quote;
        while (i < len) {
            if (src[i] == '\\' && i + 1 < len) { i += 2; continue; }
            if (i + 2 < len && src[i] == q && src[i+1] == q && src[i+2] == q) {
                i += 3; st->triple_quote = 0; break;
            }
            i++;
        }
        *pi = i;
        return CTK_STRING;
    }

    u8 c = src[i];
    bool c_family = (st->lang == CODE_LANG_C || st->lang == CODE_LANG_RUST ||
                     st->lang == CODE_LANG_JS || st->lang == CODE_LANG_GO);
    bool hash_cmt = (st->lang == CODE_LANG_PYTHON || st->lang == CODE_LANG_SHELL);

    if (c_family && i + 1 < len && c == '/' && src[i+1] == '/') {
        while (i < len && src[i] != '\n') i++;
        *pi = i; return CTK_COMMENT;
    }
    if (c_family && i + 1 < len && c == '/' && src[i+1] == '*') {
        st->in_block_comment = true;
        i += 2;
        while (i < len) {
            if (i + 1 < len && src[i] == '*' && src[i+1] == '/') {
                i += 2; st->in_block_comment = false; break;
            }
            i++;
        }
        *pi = i; return CTK_COMMENT;
    }
    if (hash_cmt && c == '#') {
        while (i < len && src[i] != '\n') i++;
        *pi = i; return CTK_COMMENT;
    }
    /* C #directive — only when '#' is the first non-blank on the line. */
    if (st->lang == CODE_LANG_C && c == '#') {
        u32 j = i;
        while (j > 0 && src[j-1] != '\n') {
            if (src[j-1] != ' ' && src[j-1] != '\t') break;
            j--;
        }
        if (j == 0 || src[j-1] == '\n') {
            while (i < len && src[i] != '\n') i++;
            *pi = i; return CTK_PREPROC;
        }
    }

    /* Python triple-quoted strings — try before single quote. */
    if (st->lang == CODE_LANG_PYTHON && i + 2 < len &&
        (c == '"' || c == '\'') && src[i+1] == c && src[i+2] == c) {
        u8 q = c;
        st->triple_quote = q;
        i += 3;
        while (i < len) {
            if (src[i] == '\\' && i + 1 < len) { i += 2; continue; }
            if (i + 2 < len && src[i] == q && src[i+1] == q && src[i+2] == q) {
                i += 3; st->triple_quote = 0; break;
            }
            i++;
        }
        *pi = i; return CTK_STRING;
    }

    if (c == '"' || c == '\'') {
        u8 q = c;
        i++;
        while (i < len && src[i] != '\n') {
            if (src[i] == '\\' && i + 1 < len) { i += 2; continue; }
            if (src[i] == q) { i++; break; }
            i++;
        }
        *pi = i; return CTK_STRING;
    }

    if (c >= '0' && c <= '9') {
        while (i < len) {
            u8 d = src[i];
            if ((d >= '0' && d <= '9') || (d >= 'a' && d <= 'f') || (d >= 'A' && d <= 'F') ||
                d == 'x' || d == 'X' || d == '.' || d == '_' ||
                d == 'u' || d == 'U' || d == 'l' || d == 'L' ||
                d == 'p' || d == 'P') { i++; continue; }
            break;
        }
        *pi = i; return CTK_NUMBER;
    }

    if (code_ident_start(c)) {
        u32 s = i;
        while (i < len && code_ident_cont(src[i])) i++;
        *pi = i;
        if (code_is_keyword(st->lang, src + s, i - s)) return CTK_KEYWORD;
        return CTK_DEFAULT;
    }

    *pi = i + 1;
    return CTK_DEFAULT;
}

static f32 render_math_block(const MdBlock *b, MdRenderCtx *ctx,
                             f32 x, f32 y, f32 max_x);
static f32 render_diagram_block(const MdBlock *b, MdRenderCtx *ctx,
                                f32 x, f32 y, f32 max_x);

static f32 render_code(const MdBlock *b, MdRenderCtx *ctx,
                        f32 x, f32 y, f32 max_x) {
    f32 cw = ctx->cw, ch = ctx->ch;

    /* A ```math / ```latex / ```tex fence renders as display math. */
    if (b->info.len && (slice_eq_ci_cstr(b->info, "math") ||
                        slice_eq_ci_cstr(b->info, "latex") ||
                        slice_eq_ci_cstr(b->info, "tex")))
        return render_math_block(b, ctx, x, y, max_x);

    /* A ```mermaid fence with a supported diagram type renders natively;
     * anything we can't lay out falls through to the code path below. */
    if (b->info.len && slice_eq_ci_cstr(b->info, "mermaid") &&
        md_diagram_probe(b->raw.data, b->raw.len) != MD_DIAGRAM_NONE)
        return render_diagram_block(b, ctx, x, y, max_x);

    f32 outer_pad_x = 6.0f;
    f32 inner_pad_y = ch * 0.25f;
    /* Always reserve a header band: it carries the language label (when set)
     * and the Copy button, so every fenced block gets a consistent chrome. */
    f32 header_h    = ch + 6.0f;

    /* Reserve the last column for the wrap-continuation arrow so it stays
     * inside the panel instead of bleeding past max_x. */
    u32 max_cells = cw > 0 ? (u32)((max_x - x) / cw) : 1u;
    if (max_cells < 8) max_cells = 8;
    if (max_cells > 1) max_cells--;

    /* Off-screen blocks pay only a cheap line count: the visible-band
     * culler already approximates with a 4×ch slack, and the y-advance
     * is the only output that matters when nothing is drawn. */
    u32 lines = 0;
    if (ctx->measure_only && !ctx->measure_exact) {
        lines = 1;
        for (u32 i = 0; i < b->raw.len; i++) if (b->raw.data[i] == '\n') lines++;
    } else {
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
    if (ctx->measure_only) return y + panel_h + ch * 0.3f;

    Color panel_bg     = ctx->_code_block_panel_bg;
    Color accent       = color_with_alpha(ctx->theme->ansi[3], 0.45f);
    Color code_text_fg = ctx->_code_block_text_fg;

    f32 cb_r  = ctx->dpi * 5.0f;
    f32 pnl_x = x - outer_pad_x;
    f32 pnl_w = max_x - x + 2 * outer_pad_x;
    cr_draw_rrect(ctx, pnl_x, y, pnl_w, panel_h, panel_bg, cb_r);
    /* Left accent bar — inset + rounded so it sits within the rounded panel. */
    cr_draw_rrect(ctx, pnl_x, y + cb_r, 3, panel_h - 2 * cb_r, accent, 1.5f * ctx->dpi);

    /* Header band: top-rounded tint matching the panel, thin bottom border.
     * Body content begins below it so the label never overlaps code text. */
    if (header_h > 0) {
        if (!ctx->measure_only) {
            /* Inset past the 3px accent bar (top-left square, top-right rounded
             * to meet the panel corner) so the band never overlaps the accent. */
            renderer_draw_rrect(ctx->r, pnl_x + 3, y, pnl_w - 3, header_h,
                          color_with_alpha(ctx->theme->ansi[8], 0.18f),
                          0.0f, cb_r, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
            renderer_draw_rrect_simple(ctx->r, pnl_x + 3, y + header_h - 1, pnl_w - 3,
                          fmaxf(1.0f, ctx->dpi * 0.5f),
                          color_with_alpha(ctx->theme->border, 0.5f), 0.0f);
        }

        f32 ly = y + (header_h - ch) * 0.5f;
        /* Language label — left. */
        if (b->info.len) {
            InlineRender ir = ir_make(ctx, ly, x + 4, max_x, cw, ch);
            ir.fg = color_with_alpha(ctx->theme->ansi[6], 0.9f);
            ir_emit_bytes(&ir, b->info, ir.fg, false, false);
        }
        /* Copy button — right: a small chip plus a recorded click target whose
         * `url` is the code text (consumed by fb_md_hit_copy on click). */
        if (!ctx->measure_only) {
            const char *cap = "Copy";
            f32 cap_w  = (f32)strlen(cap) * cw;
            f32 cpad   = 6.0f;
            f32 chip_w = cap_w + 2.0f * cpad;
            f32 chip_h = ch + 2.0f;
            f32 chip_x = max_x - chip_w;
            f32 chip_y = y + (header_h - chip_h) * 0.5f;
            cr_draw_rrect(ctx, chip_x, chip_y, chip_w, chip_h,
                          color_with_alpha(ctx->theme->ansi[8], 0.30f), chip_h * 0.4f);
            InlineRender ir = ir_make(ctx, ly, chip_x + cpad, max_x + 8, cw, ch);
            ir.fg = color_with_alpha(ctx->theme->fg, 0.82f);
            MdSlice cs = { (const u8 *)cap, (u32)strlen(cap) };
            ir_emit_bytes(&ir, cs, ir.fg, false, false);
            if (ctx->link_rects && ctx->link_rect_count &&
                *ctx->link_rect_count < ctx->link_rect_cap) {
                ctx->link_rects[*ctx->link_rect_count] = (MdLinkRect){
                    chip_x, chip_y, chip_w, chip_h,
                    b->raw.data, b->raw.len, MD_LINKRECT_COPY };
                (*ctx->link_rect_count)++;
            }
        }
    }

    f32 line_y = y + header_h + inner_pad_y;
    Color cont_fg = color_with_alpha(code_text_fg, 0.45f);
    CodeLexState lex = { .lang = code_lang_from_info(b->info.data, b->info.len) };
    bool highlight = (lex.lang != CODE_LANG_NONE);

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

        f32 cx = x;
        u32 cells_emitted = 0;
        u32 j = line_start;
        while (j < line_end) {
            /* Pick the next chunk + colour. With no language we still walk
             * one codepoint at a time so the existing tab/wrap pacing stays
             * intact — only the colour is the change. */
            u32 chunk_end;
            Color tok_color = code_text_fg;
            if (highlight) {
                u32 tj = j;
                CodeTokKind kind = code_eat_token(&lex, b->raw.data, line_end, &tj);
                if (tj <= j) tj = j + 1;
                chunk_end = tj;
                switch (kind) {
                case CTK_KEYWORD: tok_color = ctx->_code_kw;  break;
                case CTK_STRING:  tok_color = ctx->_code_str; break;
                case CTK_COMMENT: tok_color = ctx->_code_cmt; break;
                case CTK_NUMBER:  tok_color = ctx->_code_num; break;
                case CTK_PREPROC: tok_color = ctx->_code_pre; break;
                default:          tok_color = code_text_fg;   break;
                }
            } else {
                u32 cp_probe = 0;
                u32 n_probe  = utf8_decode(b->raw.data + j, line_end - j, &cp_probe);
                chunk_end = j + (n_probe ? n_probe : 1);
            }

            u32 t = j;
            while (t < chunk_end) {
                u32 cp = 0;
                u32 n = utf8_decode(b->raw.data + t, chunk_end - t, &cp);
                if (n == 0) { t++; continue; }
                t += n;

                if (cp == '\t') {
                    u32 step = MD_CODE_TAB_WIDTH - (cells_emitted % MD_CODE_TAB_WIDTH);
                    while (step > 0) {
                        if (cells_emitted >= max_cells) {
                            cr_push_glyph(ctx, cx, line_y, 0x21AA, cont_fg);
                            line_y += ch;
                            cx = x;
                            cells_emitted = 0;
                        }
                        u32 avail = max_cells - cells_emitted;
                        u32 take  = step < avail ? step : avail;
                        cx            += (f32)take * cw;
                        cells_emitted += take;
                        step          -= take;
                    }
                    continue;
                }
                if (cp < 32) continue;

                if (cells_emitted >= max_cells) {
                    cr_push_glyph(ctx, cx, line_y, 0x21AA, cont_fg);
                    line_y += ch;
                    cx = x;
                    cells_emitted = 0;
                }
                cr_push_glyph(ctx, cx, line_y, cp, tok_color);
                cx += cw;
                cells_emitted++;
            }
            j = chunk_end;
        }
        line_y += ch;
        if (i < b->raw.len) i++;
    }
    return y + panel_h + ch * 0.3f;
}

static f32 render_list_item(const MdBlock *b, MdRenderCtx *ctx,
                             f32 x, f32 y, f32 max_x) {
    f32 cw = ctx->cw, ch = ctx->ch;
    f32 indent = (f32)b->list_depth * 2 * cw;
    f32 marker_x = x + indent;
    f32 text_x   = marker_x + 2 * cw;
    if (b->list_ordered) {
        char num[12];
        i32 n = snprintf(num, sizeof num, "%u.",
                         (unsigned)(b->list_index ? b->list_index : 1));
        f32 nx = marker_x;
        for (i32 k = 0; k < n; k++) {
            cr_push_glyph(ctx, nx, y, (u32)(u8)num[k], ctx->theme->ansi[12]);
            nx += cw;
        }
        text_x = marker_x + (f32)(n + 1) * cw;
    } else {
        cr_push_glyph(ctx, marker_x, y, 0x2022, ctx->theme->ansi[12]);
    }
    InlineRender ir = ir_make(ctx, y, text_x, max_x, cw, ch);
    ir_emit_inlines(&ir, b->inlines, b->inline_count);
    return ir_finish(&ir) + ch * 0.15f;
}

static f32 render_task_item(const MdBlock *b, MdRenderCtx *ctx,
                             f32 x, f32 y, f32 max_x) {
    f32 cw = ctx->cw, ch = ctx->ch;
    f32 indent = (f32)b->list_depth * 2 * cw;
    f32 box_x  = x + indent;
    f32 text_x = box_x + 2 * cw;
    /* Checkbox: outline + fill if checked */
    f32 box_sz = ch * 0.65f;
    f32 box_y  = y + (ch - box_sz) * 0.5f;
    Color outline = color_with_alpha(ctx->theme->fg, 0.65f);
    /* outline */
    cr_draw_rect(ctx, box_x, box_y, box_sz, 1, outline);
    cr_draw_rect(ctx, box_x, box_y + box_sz - 1, box_sz, 1, outline);
    cr_draw_rect(ctx, box_x, box_y, 1, box_sz, outline);
    cr_draw_rect(ctx, box_x + box_sz - 1, box_y, 1, box_sz, outline);
    u8 st = b->task_state;
    bool done = (st == 'x' || st == 'X');
    bool cancelled = (st == '-');
    if (st != 0) {
        Color fill = cancelled ? color_with_alpha(ctx->theme->fg, 0.30f)
                   : done       ? ctx->theme->ansi[10]
                                : color_with_alpha(ctx->theme->ansi[11], 0.85f);
        cr_draw_rect(ctx, box_x + 2, box_y + 2,
                           box_sz - 4, box_sz - 4, fill);
        /* ✓ for done, ✕ for cancelled, else the literal marker (/, >, …). */
        u32 mark = done ? 0x2713 : (cancelled ? 0x2715 : (u32)st);
        cr_push_glyph(ctx, box_x + 1, box_y - ch * 0.2f, mark, ctx->theme->bg);
    }
    /* Emit the clickable checkbox rect (real draw pass only — measure passes
     * must not record duplicate/extra rects). */
    if (!ctx->measure_only && ctx->task_rects && ctx->task_rect_count &&
        *ctx->task_rect_count < ctx->task_rect_cap) {
        ctx->task_rects[*ctx->task_rect_count] = (MdTaskRect){
            .x = box_x, .y = box_y, .w = box_sz, .h = box_sz, .src_off = b->src_off,
        };
        (*ctx->task_rect_count)++;
    }
    InlineRender ir = ir_make(ctx, y, text_x, max_x, cw, ch);
    if (done || cancelled) ir.fg = ctx->theme->ansi[8];   /* dim done/cancelled */
    ir_emit_inlines(&ir, b->inlines, b->inline_count);
    return ir_finish(&ir) + ch * 0.15f;
}

/* Forward decl: callout/quote bodies recurse through the block dispatcher to
 * render their parsed child blocks (lists, paragraphs, nested callouts). */
static f32 render_block(const MdBlock *b, MdRenderCtx *ctx,
                        f32 x, f32 y, f32 max_x);

/* Render a block's child blocks stacked vertically from `y`, returning the y
 * just below the last child. */
static f32 render_children(const MdBlock *b, MdRenderCtx *ctx,
                           f32 x, f32 y, f32 max_x) {
    bool save_nested = ctx->in_nested_block;
    ctx->in_nested_block = true;
    f32 cy = y;
    for (u32 i = 0; i < b->child_count; i++)
        cy = render_block(&b->children[i], ctx, x, cy, max_x);
    ctx->in_nested_block = save_nested;
    return cy;
}

static f32 render_quote(const MdBlock *b, MdRenderCtx *ctx,
                         f32 x, f32 y, f32 max_x) {
    f32 cw = ctx->cw, ch = ctx->ch;
    f32 bar_w = fmaxf(2.0f, ctx->dpi * 1.5f);
    Color bar = color_with_alpha(ctx->theme->ansi[12], 0.7f);
    f32 start_y = y;
    f32 inner_x = x + bar_w + 8;
    f32 end_y;
    f32 bar_end;
    if (b->child_count) {
        end_y = render_children(b, ctx, inner_x, start_y, max_x);
        /* render_children includes the last child's trailing inter-block gap;
         * stop the accent bar just below the content so it doesn't dangle. */
        bar_end = end_y - ch * 0.3f;
        if (bar_end < start_y) bar_end = end_y;
    } else {
        InlineRender ir = ir_make(ctx, y, inner_x, max_x, cw, ch);
        ir.fg = color_dim(ctx->theme->fg, 0.85f);
        ir_emit_inlines(&ir, b->inlines, b->inline_count);
        end_y = ir_finish(&ir);
        bar_end = end_y;
    }
    cr_draw_rect(ctx, x, start_y, bar_w, bar_end - start_y, bar);
    return end_y + ch * 0.25f;
}

/* Lay out (and, unless ctx->measure_only, draw) a callout's title row + body,
 * returning the y just below the content. Factored out so render_callout can
 * run a measure pass to size the card, paint the background, then re-run in
 * draw mode so the content lands on top of (and nested cards behind) the bg. */
static f32 callout_layout(const MdBlock *b, MdRenderCtx *ctx,
                          f32 inner_x, f32 start_y, f32 max_x, Color title_fg) {
    f32 cw = ctx->cw, ch = ctx->ch;
    f32 line_y = start_y + 4;

    /* Title row */
    const char *gly = callout_glyph((MdCalloutKind)b->callout_kind);
    cr_push_glyph(ctx, inner_x, line_y, (u32)gly[0], title_fg);
    f32 title_text_x = inner_x + cw * 1.5f;
    InlineRender tir = ir_make(ctx, line_y, title_text_x, max_x - 8, cw, ch);
    tir.fg = title_fg;
    tir.fg_bright = color_emphasize(title_fg, ctx->theme->bg, 0.2f);
    if (b->info.len) {
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
    line_y = ir_finish(&tir);

    /* Body — prefer structurally-parsed child blocks; fall back to a flat
     * inline run for callouts that parsed without children. */
    if (b->child_count) {
        line_y += ch * 0.25f;
        line_y = render_children(b, ctx, inner_x, line_y, max_x - 8);
    } else if (b->inline_count) {
        line_y += ch * 0.25f;
        InlineRender bir = ir_make(ctx, line_y, inner_x, max_x - 8, cw, ch);
        ir_emit_inlines(&bir, b->inlines, b->inline_count);
        line_y = ir_finish(&bir);
    }
    return line_y;
}

static f32 render_callout(const MdBlock *b, MdRenderCtx *ctx,
                           f32 x, f32 y, f32 max_x) {
    f32 ch = ctx->ch;
    Color accent = callout_color(ctx->theme, (MdCalloutKind)b->callout_kind);
    Color body_bg = color_with_alpha(accent, 0.12f);
    Color title_fg = accent;

    f32 bar_w   = fmaxf(3.0f, ctx->dpi * 2.0f);
    f32 inner_x = x + bar_w + 10;
    f32 start_y = y + 4;
    f32 co_r = ctx->dpi * 5.0f;

    /* Measure the content first (no draws) to size the card. measure_exact
     * forces nested code blocks to report their wrapped height so the card
     * doesn't clip code that soft-wraps inside the callout. */
    bool save_measure = ctx->measure_only;
    bool save_exact   = ctx->measure_exact;
    ctx->measure_only  = true;
    ctx->measure_exact = true;
    f32 end_y = callout_layout(b, ctx, inner_x, start_y, max_x, title_fg) + 6;
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
    return end_y + ch * 0.3f;
}

static f32 render_hr(MdRenderCtx *ctx, f32 x, f32 y, f32 max_x) {
    f32 thickness = fmaxf(1.0f, ctx->dpi * 0.6f);
    cr_draw_rect(ctx, x, y + ctx->ch * 0.5f,
                       max_x - x, thickness,
                       color_with_alpha(ctx->theme->border, 0.7f));
    return y + ctx->ch;
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
    f32 cw = ctx->cw, ch = ctx->ch;
    /* count lines */
    u32 lines = 1;
    for (u32 i = 0; i < b->raw.len; i++) if (b->raw.data[i] == '\n') lines++;
    f32 panel_h = lines * ch + ch * 0.3f;
    Color panel_bg = color_with_alpha(ctx->theme->ansi[8], 0.18f);
    cr_draw_rrect(ctx, x - 6, y, max_x - x + 12, panel_h, panel_bg, ctx->dpi * 5.0f);

    /* "Properties" badge — rounded pill */
    const char *label = "Properties";
    f32 label_w = (f32)strlen(label) * cw + 8;
    cr_draw_rrect(ctx, max_x - label_w, y + 4, label_w, ch,
                       color_with_alpha(ctx->theme->ansi[8], 0.4f), ch * 0.5f);
    {
        InlineRender ir = ir_make(ctx, y + 4, max_x - label_w + 4,
                                  max_x + 4, cw, ch);
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
    f32 line_y = y + ch * 0.15f;
    Color key_fg = color_with_alpha(ctx->theme->ansi[12], 0.9f);
    Color val_fg = color_dim(ctx->theme->fg, 0.88f);

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
                if (cx >= key_limit - cw) { cr_push_glyph(ctx, cx, line_y, 0x2026, key_fg); break; }
                if (cp >= 32) cr_push_glyph(ctx, cx, line_y, cp, key_fg);
                cx += cw; j += n;
            }
            /* value column (skip ':' + leading spaces/tabs) */
            u32 vstart = (u32)kc + 1;
            while (vstart < line_end &&
                   (b->raw.data[vstart] == ' ' || b->raw.data[vstart] == '\t')) vstart++;
            f32 vx = val_x;
            for (u32 j = vstart; j < line_end && vx < max_x; ) {
                u32 cp = 0; u32 n = utf8_decode(b->raw.data + j, line_end - j, &cp);
                if (n == 0) { j++; continue; }
                if (cp >= 32) cr_push_glyph(ctx, vx, line_y, cp, val_fg);
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
    return y + panel_h + ch * 0.4f;
}

static u32 table_cell_text_cells(const MdTableCell *cell) {
    u32 cells = 0;
    if (!cell) return 0;
    for (u32 k = 0; k < cell->inline_count; k++) {
        cells += (u32)utf8_len(cell->inlines[k].text.data,
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

static f32 render_table(const MdBlock *b, MdRenderCtx *ctx,
                         f32 x, f32 y, f32 max_x) {
    f32 cw = ctx->cw, ch = ctx->ch;
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
            for (u32 k = 0; k < cell->inline_count; k++) {
                const MdInline *in = &cell->inlines[k];
                w += (u32)utf8_len(in->text.data, in->text.len);
                if (in->kind == MD_INLINE_TAG) w += 1;
            }
            if (w > col_w[c]) col_w[c] = w;
        }
    }
    /* Add 2-cell padding per column, fit into available width. */
    u32 sum = 0;
    for (u16 c = 0; c < b->cols && c < 32; c++) sum += col_w[c] + 2;
    f32 avail = (max_x - x) / cw;
    if (sum > 0 && (f32)sum > avail) {
        f32 ratio = avail / (f32)sum;
        for (u16 c = 0; c < b->cols && c < 32; c++) {
            u32 nw = (u32)(((f32)col_w[c] + 2) * ratio);
            col_w[c] = nw < 3 ? 3 : nw - 2;
        }
    }

    /* Geometry pre-pass: the total table extent is needed to draw the outer
     * frame in a single rect before cells get filled in. */
    f32 cell_pad_y = ch * 0.18f;          /* vertical breathing room per row */
    f32 sep_thick  = fmaxf(1.0f, ctx->dpi * 0.6f);
    f32 head_under = fmaxf(2.0f, ctx->dpi * 1.2f);  /* bolder header underline */
    f32 frame_th   = fmaxf(1.0f, ctx->dpi * 0.6f);

    /* Compute total content width (sum of columns) so we can clamp the
     * outer frame and vertical separators to the actual table extent
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
        return y + tbl_h_m + ch * 0.5f;
    }

    /* Color palette derived from the theme. We use border at full alpha so
     * the lines actually pop on dark themes (where border alpha=1 already
     * but its luminance is low — we boost it slightly for contrast). */
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
    Color head_bg  = color_with_alpha(ctx->theme->ansi[8], 0.30f);
    Color head_underline_c = ctx->theme->ansi[12];   /* bright blue accent */
    head_underline_c.a = 1.0f;

    /* --- Background & frame ------------------------------------------------ */
    /* Header background. */
    cr_draw_rect(ctx, x, head_y0, total_w, header_h, head_bg);
    /* Header underline (bolder than borders so the header reads). */
    cr_draw_rect(ctx, x, head_y1, total_w, head_under, head_underline_c);

    /* Outer frame: top, bottom, left, right. */
    f32 tbl_h = (head_y1 - head_y0) + head_under + (body_y1 - body_y0);
    cr_draw_rect(ctx, x, head_y0,                total_w, frame_th, line_strong);   /* top */
    cr_draw_rect(ctx, x, head_y0 + tbl_h - frame_th, total_w, frame_th, line_strong); /* bottom */
    cr_draw_rect(ctx, x, head_y0,                frame_th, tbl_h, line_strong);     /* left */
    cr_draw_rect(ctx, x + total_w - frame_th, head_y0, frame_th, tbl_h, line_strong); /* right */

    /* Vertical column separators: skip first (= left frame) and last
     * (= right frame), draw between every adjacent pair. */
    {
        f32 cx_acc = x;
        for (u16 c = 0; c + 1 < b->cols && c + 1 < 32; c++) {
            cx_acc += (col_w[c] + 2) * cw;
            if (cx_acc - x >= total_w) break;
            cr_draw_rect(ctx, cx_acc - sep_thick * 0.5f, head_y0,
                               sep_thick, tbl_h, line_strong);
        }
    }

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
            cr_draw_rect(ctx, x + frame_th, sy,
                                   total_w - 2 * frame_th, sep_thick, line_dim);
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
            ir_emit_inlines(&ir, cell->inlines, cell->inline_count);

            cell_x += col_px;
        }
        if (r == 0) row_y = body_y0;
        else        row_y += row_content_h + cell_pad_y;
    }
    return head_y0 + tbl_h + ch * 0.5f;
}

static f32 render_html(const MdBlock *b, MdRenderCtx *ctx,
                        f32 x, f32 y, f32 max_x) {
    /* Render passthrough as dim mono. */
    f32 cw = ctx->cw, ch = ctx->ch;
    Color fg = color_dim(ctx->theme->fg, 0.6f);
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
    return line_y;
}

/* Inline IMAGE/EMBED rendering: walk a paragraph's inline runs and, if
 * any are image/embed, draw the image at block-level dimensions and
 * suppress the inline placeholder. Returns the new y after image. We
 * detect images by scanning the block's first IMAGE/EMBED inline. */
static f32 render_image_block(const MdInline *img, MdRenderCtx *ctx,
                               f32 x, f32 y, f32 max_x) {
    /* Media/document embed (mp4/mp3/pdf/…) → a labelled chip rather than an
     * attempted image decode (which would render "[image not found]"). */
    {
        MdSlice tgt = img->url.len ? img->url : img->alt;
        const char *mlabel = media_chip_label(tgt);
        if (mlabel) {
            f32 cw = ctx->cw, ch = ctx->ch;
            const u8 *nm = tgt.data; u32 nl = tgt.len;
            for (i32 k = (i32)tgt.len - 1; k >= 0; k--)
                if (tgt.data[k] == '/') { nm = tgt.data + k + 1; nl = tgt.len - (u32)(k + 1); break; }
            u32 lbl = (u32)strlen(mlabel);
            /* Cap the basename so the whole chip fits on one line — the chip
             * height is fixed at one row, so a wrapped name would overlap the
             * next block. (utf8-naive truncation; good enough for filenames.) */
            u32 avail = (u32)((max_x - x) / cw);
            if (avail < lbl + 6) avail = lbl + 6;
            u32 max_name = avail - lbl - 4;
            if (nl > max_name) nl = max_name;
            f32 chip_w = (f32)(lbl + 2 + nl + 2) * cw;
            if (chip_w > max_x - x) chip_w = max_x - x;
            f32 chip_h = ch + 8.0f;
            if (!ctx->measure_only) {
                cr_draw_rrect(ctx, x, y, chip_w, chip_h,
                              color_with_alpha(ctx->theme->ansi[5], 0.16f), ctx->dpi * 5.0f);
                InlineRender ir = ir_make(ctx, y + 4, x + cw, x + chip_w + cw, cw, ch);
                ir_link_begin(&ir, tgt, MD_LINKRECT_EMBED);
                MdSlice ls = { (const u8 *)mlabel, lbl };
                ir_emit_bytes(&ir, ls, ctx->theme->ansi[13], false, false);
                ir.cx += cw;   /* gap */
                MdSlice ns = { nm, nl };
                ir_emit_bytes(&ir, ns, color_dim(ctx->theme->fg, 0.85f), false, false);
                ir_link_end(&ir);
            }
            return y + chip_h + ch * 0.4f;
        }
    }
    if (!ctx->images || img->url.len == 0) {
        /* placeholder badge */
        InlineRender ir = ir_make(ctx, y, x, max_x, ctx->cw, ctx->ch);
        ir.fg = ctx->theme->ansi[8];
        const char *p = "[image]";
        MdSlice s = { (const u8 *)p, (u32)strlen(p) };
        ir_emit_bytes(&ir, s, ir.fg, false, false);
        return ir_finish(&ir) + ctx->ch * 0.4f;
    }
    const MdImageEntry *e = md_image_cache_get(ctx->images, ctx->base_dir,
                                               (const char *)img->url.data,
                                               img->url.len);
    if (!e || e->failed || !e->rgba) {
        Color fg = color_with_alpha(ctx->theme->ansi[9], 0.85f);
        InlineRender ir = ir_make(ctx, y, x, max_x, ctx->cw, ctx->ch);
        ir.fg = fg;
        const char *prefix = "[image not found: ";
        MdSlice ps = { (const u8 *)prefix, (u32)strlen(prefix) };
        ir_link_begin(&ir, img->url, MD_LINKRECT_IMAGE);
        ir_emit_bytes(&ir, ps, fg, false, false);
        ir_emit_bytes(&ir, img->url, fg, false, false);
        const char *suffix = "]";
        MdSlice ss = { (const u8 *)suffix, 1 };
        ir_emit_bytes(&ir, ss, fg, false, false);
        ir_link_end(&ir);
        return ir_finish(&ir) + ctx->ch * 0.4f;
    }
    /* Fit to half viewport width keeping aspect ratio, capped at 480 px. */
    f32 max_w = fminf((max_x - x), 480.0f * ctx->dpi);
    f32 scale = max_w / (f32)e->w;
    if (scale > 1.0f) scale = 1.0f;
    f32 w = (f32)e->w * scale;
    f32 h = (f32)e->h * scale;
    /* Off-screen (measure pass): only the height matters. Skip the texture
     * bind + draw + batch-break that a hidden image would otherwise cost
     * every frame. renderer_draw_image_cached already flushes rects+glyphs
     * at entry, so no caller-side flush is needed for the visible path. */
    if (ctx->measure_only)
        return y + h + ctx->ch * 0.4f;
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
    return y + h + ctx->ch * 0.4f;
}

/* Display math block ($$…$$ or a ```math fence). TeX is in b->raw. Centered. */
static f32 render_math_block(const MdBlock *b, MdRenderCtx *ctx,
                             f32 x, f32 y, f32 max_x) {
    f32 pad = 0.5f * ctx->ch;
    f32 w = 0, asc = 0, desc = 0;
    md_math_measure(b->raw.data, b->raw.len, ctx->cw, ctx->ch, true, &w, &asc, &desc);
    if (w <= 0.0f)              /* nothing measurable → show the raw TeX as code */
        return render_code(b, ctx, x, y, max_x);
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
    f32 pad = 0.5f * ctx->ch;
    f32 w = 0, h = 0;
    md_diagram_measure(b->raw.data, b->raw.len, max_x - x, ctx->cw, ctx->ch, &w, &h);
    if (w <= 0.0f || h <= 0.0f)        /* nothing laid out → raw code fallback */
        return render_code(b, ctx, x, y, max_x);
    if (!ctx->measure_only)
        md_diagram_render(ctx->r, ctx->theme, b->raw.data, b->raw.len,
                          x, y + pad, max_x - x, ctx->cw, ctx->ch);
    return y + pad + h + pad;
}

static f32 render_block(const MdBlock *b, MdRenderCtx *ctx,
                         f32 x, f32 y, f32 max_x) {
    /* Standalone image / embed paragraphs: detect when paragraph contains
     * a single IMAGE or EMBED inline. */
    if (b->kind == MD_BLOCK_PARAGRAPH && b->inline_count >= 1) {
        const MdInline *first = &b->inlines[0];
        if ((first->kind == MD_INLINE_IMAGE || first->kind == MD_INLINE_EMBED) &&
            b->inline_count == 1) {
            return render_image_block(first, ctx, x, y, max_x);
        }
    }

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
    case MD_BLOCK_DEF_TERM:    return render_def_term(b, ctx, x, y, max_x);
    case MD_BLOCK_DEF_DESC:    return render_def_desc(b, ctx, x, y, max_x);
    default:                   return y + ctx->ch;
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

f32 md_render(MdDoc *doc, MdRenderCtx *ctx) {
    if (!doc || !ctx || !ctx->r) return 0.0f;

    /* Reset the link-rect collector at the top of each render — the buffer
     * is owned by the caller (typically reused across frames). */
    if (ctx->link_rects && ctx->link_rect_count) *ctx->link_rect_count = 0;
    if (ctx->glyph_rects && ctx->glyph_rect_count) *ctx->glyph_rect_count = 0;
    if (ctx->outline && ctx->outline_count) *ctx->outline_count = 0;
    if (ctx->task_rects && ctx->task_rect_count) *ctx->task_rect_count = 0;

    /* Pre-compute the per-frame palette + every text/surface color the
     * block renderers consume. Done once here so ir_make and render_code
     * stay cheap — they used to call chrome_palette_for + bisecting WCAG
     * contrast checks per block (and per cell, in tables), which on a
     * mid-sized doc produced thousands of powf() per frame. */
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
        ctx->_heading_fg     = color_ensure_contrast(t->ansi[12], t->bg, 4.5f);
        ctx->_heading_bright = color_emphasize(ctx->_heading_fg, t->bg, 0.15f);
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
        ctx->_derived_ready  = true;
    }

    f32 max_x = ctx->x + ctx->w;
    f32 y_top = ctx->y;
    f32 y_bottom = ctx->y + ctx->h;
    f32 scroll = ctx->scroll_y < 0.0f ? 0.0f : ctx->scroll_y;

    /* Virtual cursor walks the entire document; scroll offset is subtracted
     * so on-screen y = virtual_y - scroll. Blocks whose computed band sits
     * outside [y_top, y_bottom) are walked in measure-only mode so we
     * still get their height for the total content_px return value but
     * don't pay the per-glyph push cost or burn batched quads off-screen.
     * That keeps stress.md (~50 blocks, hundreds of inline tokens) snappy
     * during scroll, since each frame only emits glyphs for ~3-4 blocks. */
    /* Bump the layout generation when the wrap width or cell metrics change;
     * any block height cached under the previous layout is then stale. */
    if (doc->layout_w != ctx->w || doc->layout_cw != ctx->cw ||
        doc->layout_ch != ctx->ch) {
        doc->layout_w  = ctx->w;
        doc->layout_cw = ctx->cw;
        doc->layout_ch = ctx->ch;
        doc->layout_gen++;
        if (doc->layout_gen == 0) doc->layout_gen = 1;  /* 0 == "never cached" */
    }

    /* Walk the whole document; on-screen blocks emit glyphs, off-screen blocks
     * only need their height to advance the virtual cursor. Block heights are
     * invariant for a fixed (w, cw, ch), so a block that's off-screen with a
     * height already cached for this layout skips the per-codepoint measure
     * walk entirely — steady-state scroll becomes O(blocks), not O(glyphs). */
    f32 virt_y = 0.0f;
    bool save_measure = ctx->measure_only;
    for (u32 i = 0; i < doc->block_count; i++) {
        MdBlock *blk = &doc->blocks[i];
        /* Outline capture: every heading, regardless of on/off-screen, so the
         * collected table is the full document table-of-contents. doc_y is the
         * heading's pre-scroll top. */
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
        bool maybe_visible = (effective_y < y_bottom) &&
                             (effective_y + ctx->h > y_top);
        bool need_draw = !save_measure && maybe_visible;
        f32 block_h;
        if (!need_draw && blk->cached_gen == doc->layout_gen) {
            block_h = blk->cached_h;   /* cached: no measure walk, no emit */
        } else {
            ctx->measure_only = !need_draw;
            f32 next_y = render_block(blk, ctx, ctx->x, effective_y, max_x);
            block_h = next_y - effective_y;
            blk->cached_h   = block_h;
            blk->cached_gen = doc->layout_gen;
        }
        virt_y += block_h;
    }
    ctx->measure_only = save_measure;
    renderer_flush_rects(ctx->r);
    renderer_flush_glyphs(ctx->r);
    return virt_y;
}
