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
#include "ui/markdown/md_embed.h"

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
 * nul-terminated, and may span multiple rects when a link soft-wraps.
 *
 * `anchor` carries the "#Heading" / "#^block-id" part of a wikilink or embed,
 * already split off the path by the parser. It travels with the rect so the
 * click handler can scroll the opened note to the right place without re-
 * splitting the target string — the renderer and the click dispatcher both
 * consume structured fields, neither one re-parses syntax. */
typedef struct MdLinkRect {
    f32        x, y, w, h;
    const u8  *url;
    u32        url_len;
    const u8  *anchor;
    u32        anchor_len;
    u8         anchor_is_block;
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

/* MdGlyphRect.flags — what the TEXT of the list needs to know and a rectangle
 * cannot say. The list is a picture of the page: where a line breaks on screen
 * depends on the window's width, a tab is a gap, a blank line is nothing at
 * all. A copy made from the rects alone would therefore break every wrapped
 * line, lose every tab and close every blank line up. md_glyphs_text() reads
 * these back; the selection highlight and the hit test ignore them. */
enum {
    /* First glyph of a visual row that only CONTINUES the line above it: the
     * text has no line break here. */
    MD_GLYPH_WRAP   = 1 << 0,
    /* Drawn, but not part of the document's text (the arrow a wrapped code
     * line ends with). */
    MD_GLYPH_CHROME = 1 << 1,
    /* First glyph of a block that the text sets apart from the one above with
     * a blank line. */
    MD_GLYPH_BLOCK  = 1 << 2,
    /* A space stands between this glyph and the one before it on its row,
     * though none was drawn (a list item's text after its bullet). */
    MD_GLYPH_GAP    = 1 << 3,
    /* This glyph opens the next table cell of its row: a tab before it. */
    MD_GLYPH_CELL   = 1 << 4,
};

/* A rect whose `cp` is 0 draws nothing and stands for a place in the text: a
 * blank line inside a code block, the start of a table cell. One whose `cp`
 * is '\t' is a tab's gap. Neither reaches the Renderer. */
typedef struct MdGlyphRect {
    f32 x, y, w, h;
    u32 cp;
    u32 src_off;   /* byte offset of this glyph in the parse source, or
                    * MD_GLYPH_NO_SRC. Lets callers map a source byte-range
                    * (e.g. a find match) back to on-screen glyph rects. */
    u8  flags;     /* MD_GLYPH_* */
} MdGlyphRect;

/* Click-target rect for a task-list checkbox (framebuffer px, same space +
 * per-render lifecycle as MdLinkRect). `src_off` is the byte offset of the
 * task's state char in the parse source — the click handler toggles it
 * directly, validating against the live buffer first. */
typedef struct MdTaskRect {
    f32 x, y, w, h;
    u32 src_off;
} MdTaskRect;

/* Click target for a foldable block's header (callout with a "-"/"+" marker,
 * or a <details>). `block` points into the live MdDoc, so the handler flips
 * `fold_open` directly — there is no id to look the block back up by, and the
 * document outlives the frame that recorded the rect. */
typedef struct MdFoldRect {
    f32 x, y, w, h;
    struct MdBlock *block;
} MdFoldRect;

/* Typographic scale, derived once per render from (cw, ch, dpi) and the
 * font's real atlas aspect. Everything the block renderers place vertically
 * is a multiple of one of these — no ad-hoc `ch * 0.37f` constants — so the
 * whole document's rhythm can be retuned from md_typo_make().
 *
 * The key idea: `cw` alone sets the TYPE SIZE (the renderer derives glyph
 * height from width × atlas aspect, see renderer_set_ui_scale), while the
 * `ch` handed to set_ui_scale is purely the LINE BOX the glyph is centred
 * in. That splits size from leading, which a single `ch` could not express —
 * and a line box shorter than the glyph makes the lines touch. */
typedef struct MdTypo {
    f32 cw;        /* advance per character (the type-size lever) */
    f32 gh;        /* actual drawn glyph height at that advance */
    f32 lh;        /* body line box: leading included */
    f32 unit;      /* vertical spacing unit — all block gaps are multiples */
    f32 baseline;  /* baseline offset from the top of a body line box */
    f32 indent;    /* one list-nesting step */
    f32 radius;    /* card corner radius */
    f32 hair;      /* hairline (separators, rules) */
    f32 pad_x;     /* horizontal inset inside cards */
    f32 pad_y;     /* vertical inset inside cards */
} MdTypo;

typedef struct {
    Renderer    *r;
    const Theme *theme;
    f32 x, y;          /* top-left of content area in framebuffer pixels */
    f32 w, h;          /* viewport size */
    f32 cw, ch;        /* base mono cell size in pixels */
    f32 dpi;
    f32 table_scroll_x; /* horizontal offset of overflowing tables only */
    f32 *table_overflow; /* largest visible table overflow, px */
    f32 scroll_y;      /* pixel offset from top of rendered doc */
    MdImageCache *images;
    /* Parsed documents behind `![[Note]]` transclusions. NULL disables the
     * feature and embeds fall back to a plain link chip. */
    MdEmbedCache *embeds;
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

    /* Optional anchor seek. Set `seek_block` to a block index and the renderer
     * reports that block's document-space top (pre-scroll pixels) through
     * `seek_block_y`. Free: the block walk already computes every block's y to
     * advance its virtual cursor, so this reads a number that exists rather
     * than adding a measuring pass. Resolve the index with md_embed_section(),
     * which maps a "#Heading" / "^block-id" anchor onto the block it names.
     * MD_SEEK_NONE disables it. */
#define MD_SEEK_NONE 0xFFFFFFFFu
    u32  seek_block;
    f32 *seek_block_y;

    /* Optional task-checkbox click-rect collector (see MdTaskRect). Reset at
     * the top of md_render(); one rect per visible TASK_ITEM in the draw pass. */
    MdTaskRect *task_rects;
    u32         task_rect_cap;
    u32        *task_rect_count;

    /* Optional fold-header click-rect collector (see MdFoldRect). Reset at the
     * top of md_render(); one rect per visible foldable block. */
    MdFoldRect *fold_rects;
    u32         fold_rect_cap;
    u32        *fold_rect_count;

    /* Pointer position, in the same framebuffer space as x/y. Only the code
     * block's Copy button reads it today. Leaving it zero means "the pointer
     * is at the top-left of the framebuffer", not "there is no pointer" — the
     * viewer's content never starts there, so a caller that does not plumb it
     * gets no hover rather than a wrong one. */
    f32 hover_x, hover_y;

    /* Copy-confirmation flash. `copy_flash_block` is MdBlock.raw.data for the
     * block whose Copy button was last clicked (the same pointer the click
     * rect carries, so the caller never needs a block id), and `copy_flash`
     * is its remaining strength, 1 → 0. Anything driving this must also be in
     * main.c's liu_continuous_visual(), or the flash never fades. */
    const u8 *copy_flash_block;
    f32       copy_flash;

    /* Internal: when true, render_block does layout math (advances virt_y)
     * but skips renderer draw calls. Used by md_render to compute total
     * doc height for off-screen blocks without burning GPU draw calls. */
    bool measure_only;

    /* Draw EVERY block at its document position, viewport or not — the
     * caller's scissor clips the off-screen ones on the GPU. Set for one
     * frame by the Markdown viewer's copy pass so the glyph collector covers
     * the whole document: the rects are viewport-only otherwise, and a
     * selection whose anchor scrolled out of view would resolve to the first
     * visible glyph, silently truncating the copy to what was on screen. */
    bool collect_all;

    /* Internal: when true, measure_only passes must compute EXACT heights
     * (e.g. render_code counts wrapped visual lines, not just newlines).
     * Set during the callout/quote card-sizing measure pass so the card
     * background matches the drawn content height; left false for the cheap
     * off-screen scroll-extent culling pass. */
    bool measure_exact;

    /* Internal: transclusion nesting depth. A note that embeds a note that
     * embeds the first one is a cycle; depth is what stops it. */
    u8 embed_depth;

    /* Internal: true while rendering blocks nested inside a callout/quote body.
     * Document-scope-only treatments (footnote-definition styling) are
     * suppressed for nested blocks. */
    bool in_nested_block;

    /* Per-frame palette + derived colors, computed once at the top of
     * md_render() and consumed by ir_make / render_code, so the
     * luminance helpers behind these (powf-heavy WCAG bisection) do not
     * run for every block. */
    bool  _derived_ready;
    /* Copy of MdDoc.layout_gen for this frame, so nested renderers can tell a
     * child's cached height from a stale one without carrying the doc. */
    u32   _layout_gen;
    MdTypo _t;         /* typographic scale for this render (see MdTypo) */
    bool  _is_light;
    Color _surface_raised;
    Color _fg, _fg_dim, _fg_bright;
    Color _link;
    Color _code_bg, _code_fg;
    Color _tag_fg, _tag_bg;
    Color _heading_fg, _heading_bright;
    Color _muted;      /* secondary text: captions, completed tasks, meta rows */
    Color _rule;       /* hairlines: thematic breaks, table separators */
    Color _marker;     /* list bullets and ordered-list numbers */
    Color _quote_bg, _quote_bar;
    Color _code_block_panel_bg;
    Color _code_block_text_fg;
    /* Syntax-highlight palette for fenced code blocks with a language tag.
     * Computed once per render and consumed by the token-walking emit in
     * render_code. Blocks without a language fall back to _code_block_text_fg
     * for every glyph. */
    Color _code_kw, _code_str, _code_cmt, _code_num, _code_pre;
    Color _code_typ, _code_fn;
    Color _code_ins, _code_del;   /* ```diff fences: added / removed lines */
} MdRenderCtx;

/* Render the visible window of `doc`. Caller is expected to have already
 * cleared/painted the viewport background; this routine only emits the
 * markdown content rects + glyphs + images. Returns total rendered
 * content height in pixels — caller uses this to clamp scroll bounds. */
f32 md_render(MdDoc *doc, MdRenderCtx *ctx);

/* The document's text for glyphs [lo, hi) of a list md_render filled — what a
 * copy of that selection puts on the clipboard. A line break stands where the
 * DOCUMENT has one, not where the window wrapped; tabs and blank lines inside
 * code are kept; blocks are one blank line apart.
 *
 * Writes at most `cap` bytes including the terminating NUL and returns the
 * length the whole text needs, so a caller sizes its buffer with a first call
 * of (NULL, 0). */
usize md_glyphs_text(const MdGlyphRect *rects, u32 lo, u32 hi, char *out, usize cap);

#endif /* UI_MARKDOWN_RENDER_H */
