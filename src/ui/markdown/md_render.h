/*
 * Liu - Markdown renderer.
 *
 * Consumes an MdDoc and a render context, emitting draw calls into the
 * provided Renderer. Pure: nothing about FileBrowser leaks in. Soft-wraps
 * paragraphs to fit the viewport width on every render — no virtual-line
 * bookkeeping. Heading scale comes from renderer_set_ui_scale.
 */
#ifndef UI_MARKDOWN_RENDER_H
#define UI_MARKDOWN_RENDER_H

#include "core/types.h"
#include "renderer/renderer.h"
#include "core/config.h"
#include "ui/markdown/md_ast.h"
#include "ui/markdown/md_image.h"

/* What a recorded click-rect points at, so the click dispatcher can route it
 * (open externally vs in-app .md vs filter-by-tag) without re-parsing. */
typedef enum {
    MD_LINKRECT_URL,       /* [text](url) / autolink — external or relative path */
    MD_LINKRECT_WIKILINK,  /* [[note]] / [[note|alias]] — resolve to a .md */
    MD_LINKRECT_EMBED,     /* ![[file]] */
    MD_LINKRECT_IMAGE,     /* ![alt](url) image source */
    MD_LINKRECT_TAG,       /* #tag — `url` is the tag name without '#' */
    MD_LINKRECT_COPY,      /* code-block copy button — `url` is the code text */
} MdLinkRectKind;

/* Click-target rect for a link/tag emitted during render. Coordinates are in
 * framebuffer pixels, in the same space as the supplied MdRenderCtx.x/y.
 * `url` points into the parse arena (same lifetime as MdDoc), is NOT
 * nul-terminated, and may span multiple rects when a link soft-wraps. */
typedef struct MdLinkRect {
    f32        x, y, w, h;
    const u8  *url;
    u32        url_len;
    u8         kind;       /* MdLinkRectKind */
} MdLinkRect;

/* Document-outline entry collected during render: one per heading block.
 * `doc_y` is the heading's top in document space (pre-scroll pixels) so the
 * caller can scroll directly to it. `text`/`text_len` borrow the heading's
 * first inline text run (arena-backed; same lifetime as MdDoc). */
typedef struct MdOutlineItem {
    f32        doc_y;
    u8         level;      /* 1..6 */
    const u8  *text;
    u32        text_len;
} MdOutlineItem;

/* Per-glyph rect collector for text selection in the rendered view. When
 * `glyph_rects` is non-NULL the renderer records one entry per emitted text
 * glyph, in reading order (document order, left-to-right, top-to-bottom).
 * Coordinates are framebuffer pixels (same space as MdRenderCtx.x/y — i.e.
 * scrolled screen positions). `glyph_rect_cap` bounds writes; the surplus is
 * dropped. `*glyph_rect_count` is reset at the top of md_render(). */
/* Sentinel for MdGlyphRect.src_off when a glyph has no source position
 * (synthetic markers, code-block lexer output, list bullets, …). */
#define MD_GLYPH_NO_SRC 0xFFFFFFFFu

typedef struct MdGlyphRect {
    f32 x, y, w, h;
    u32 cp;
    u32 src_off;   /* byte offset of this glyph in the parse source, or
                    * MD_GLYPH_NO_SRC. Lets callers map a source byte-range
                    * (e.g. a find match) back to on-screen glyph rects. */
} MdGlyphRect;

/* Click-target rect for a task-list checkbox (framebuffer px, same space +
 * per-render lifecycle as MdLinkRect). `src_off` is the byte offset of the
 * task's state char in the parse source — the click handler toggles it
 * directly, validating against the live buffer first. */
typedef struct MdTaskRect {
    f32 x, y, w, h;
    u32 src_off;
} MdTaskRect;

typedef struct {
    Renderer    *r;
    const Theme *theme;
    f32 x, y;          /* top-left of content area in framebuffer pixels */
    f32 w, h;          /* viewport size */
    f32 cw, ch;        /* base mono cell size in pixels */
    f32 dpi;
    f32 scroll_y;      /* pixel offset from top of rendered doc */
    MdImageCache *images;
    const char *base_dir;

    /* Parse source span (== the bytes MdSlice payloads point into, i.e. the
     * file browser's view_content). When set, the renderer fills
     * MdGlyphRect.src_off with each glyph's byte offset into this span. */
    const u8 *src_base;
    u32       src_len;

    /* Optional link-rect collector. When `link_rects` is non-NULL the
     * renderer records one rect per displayed segment of every LINK /
     * AUTOLINK / IMAGE / EMBED inline (soft-wrapped links produce one rect
     * per visual line). `link_rect_cap` bounds writes; on overflow the
     * surplus is silently dropped. `*link_rect_count` is reset at the top
     * of md_render() and updated as rects are recorded. */
    MdLinkRect *link_rects;
    u32         link_rect_cap;
    u32        *link_rect_count;

    /* Optional per-glyph rect collector for text selection (see MdGlyphRect). */
    MdGlyphRect *glyph_rects;
    u32          glyph_rect_cap;
    u32         *glyph_rect_count;

    /* Optional document-outline collector. When non-NULL, md_render records
     * one entry per heading block (in document order). `outline_cap` bounds
     * writes; `*outline_count` is reset at the top of md_render(). */
    MdOutlineItem *outline;
    u32            outline_cap;
    u32           *outline_count;

    /* Optional task-checkbox click-rect collector (see MdTaskRect). Reset at
     * the top of md_render(); one rect per visible TASK_ITEM in the draw pass. */
    MdTaskRect *task_rects;
    u32         task_rect_cap;
    u32        *task_rect_count;

    /* Internal: when true, render_block does layout math (advances virt_y)
     * but skips renderer draw calls. Used by md_render to compute total
     * doc height for off-screen blocks without burning GPU draw calls. */
    bool measure_only;

    /* Internal: when true, measure_only passes must compute EXACT heights
     * (e.g. render_code counts wrapped visual lines, not just newlines).
     * Set during the callout/quote card-sizing measure pass so the card
     * background matches the drawn content height; left false for the cheap
     * off-screen scroll-extent culling pass. */
    bool measure_exact;

    /* Internal: true while rendering blocks nested inside a callout/quote body.
     * Document-scope-only treatments (footnote-definition styling) are
     * suppressed for nested blocks. */
    bool in_nested_block;

    /* Per-frame palette + derived colors, computed once at the top of
     * md_render() and consumed by ir_make / render_code. The luminance
     * helpers behind these (powf-heavy WCAG bisection) used to run for
     * every block — for a moderate doc that's 1000s of powf/frame. */
    bool  _derived_ready;
    bool  _is_light;
    Color _surface_raised;
    Color _fg, _fg_dim, _fg_bright;
    Color _link;
    Color _code_bg, _code_fg;
    Color _tag_fg, _tag_bg;
    Color _heading_fg, _heading_bright;
    Color _code_block_panel_bg;
    Color _code_block_text_fg;
    /* Syntax-highlight palette for fenced code blocks with a language tag.
     * Computed once per render and consumed by the token-walking emit in
     * render_code. Blocks without a language fall back to _code_block_text_fg
     * for every glyph. */
    Color _code_kw, _code_str, _code_cmt, _code_num, _code_pre;
} MdRenderCtx;

/* Render the visible window of `doc`. Caller is expected to have already
 * cleared/painted the viewport background; this routine only emits the
 * markdown content rects + glyphs + images. Returns total rendered
 * content height in pixels — caller uses this to clamp scroll bounds. */
f32 md_render(MdDoc *doc, MdRenderCtx *ctx);

#endif /* UI_MARKDOWN_RENDER_H */
