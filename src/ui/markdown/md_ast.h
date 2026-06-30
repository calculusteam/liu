/*
 * Liu - Markdown AST
 *
 * Block + inline node types produced by md_parse and consumed by md_render.
 * Pure data; no functions, no renderer/theme/filebrowser dependencies.
 *
 * Lifetime: every pointer in this AST is borrowed from the parse arena.
 * MdSlice payloads point INTO the original source buffer (md_parse holds
 * the source pointer alive for the doc lifetime — file browser keeps
 * fb->view_content live until fb_close_viewer).
 */
#ifndef UI_MARKDOWN_AST_H
#define UI_MARKDOWN_AST_H

#include "core/types.h"

typedef enum {
    MD_BLOCK_PARAGRAPH,
    MD_BLOCK_HEADING,
    MD_BLOCK_CODE,
    MD_BLOCK_LIST_ITEM,        /* unordered or ordered; ordered flag in list_ordered */
    MD_BLOCK_TASK_ITEM,        /* "- [ ] x" / "- [x] x" — list_ordered always 0 */
    MD_BLOCK_QUOTE,
    MD_BLOCK_CALLOUT,          /* "> [!type] title" + body lines */
    MD_BLOCK_TABLE,
    MD_BLOCK_HR,
    MD_BLOCK_FRONTMATTER,      /* leading "---\n…\n---" YAML */
    MD_BLOCK_HTML,             /* raw passthrough (rendered as dim mono) */
    MD_BLOCK_MATH,             /* $$…$$ / \[…\] display math; TeX in `raw` */
    MD_BLOCK_DEF_TERM,         /* definition-list term (inlines) */
    MD_BLOCK_DEF_DESC          /* definition-list description (inlines) */
} MdBlockKind;

typedef enum {
    MD_INLINE_TEXT,
    MD_INLINE_BOLD,
    MD_INLINE_ITALIC,
    MD_INLINE_BOLD_ITALIC,
    MD_INLINE_CODE,
    MD_INLINE_STRIKETHROUGH,
    MD_INLINE_HIGHLIGHT,       /* ==text== */
    MD_INLINE_LINK,
    MD_INLINE_AUTOLINK,
    MD_INLINE_WIKILINK,        /* [[Page]] or [[Page|Alias]] — inert in v1 */
    MD_INLINE_IMAGE,           /* ![alt](url) */
    MD_INLINE_EMBED,           /* ![[file]] */
    MD_INLINE_TAG,             /* #tag */
    MD_INLINE_LINEBREAK,       /* hard break: trailing two spaces + \n */
    MD_INLINE_FOOTNOTE_REF,    /* [^id] */
    MD_INLINE_SUBSCRIPT,       /* ~sub~  (text holds the body) */
    MD_INLINE_SUPERSCRIPT,     /* ^sup^  (text holds the body) */
    MD_INLINE_MATH             /* $inline$ — text holds raw TeX (a leaf) */
} MdInlineKind;

/* Callout kind — derived from "> [!type]" tag. Order picks color from
 * theme->ansi[] in md_render. Unknown types fall back to MD_CALLOUT_NOTE. */
typedef enum {
    MD_CALLOUT_NOTE,
    MD_CALLOUT_INFO,
    MD_CALLOUT_TIP,
    MD_CALLOUT_SUCCESS,
    MD_CALLOUT_WARNING,
    MD_CALLOUT_DANGER,
    MD_CALLOUT_QUESTION,
    MD_CALLOUT_EXAMPLE,
    MD_CALLOUT_QUOTE,
    MD_CALLOUT_ABSTRACT,
    MD_CALLOUT_TODO,
    MD_CALLOUT_BUG,
    MD_CALLOUT_FAILURE,
    MD_CALLOUT__COUNT
} MdCalloutKind;

/* Slice — non-NUL-terminated, points into arena-owned bytes. */
typedef struct {
    const u8 *data;
    u32       len;
} MdSlice;

typedef struct MdInline {
    u8       kind;            /* MdInlineKind */
    u8       flags;            /* per-kind extension; footnote number for FOOTNOTE_REF */
    u32      child_count;      /* matches InlineVec.len (u32) — no truncation */
    MdSlice  text;             /* TEXT/CODE/AUTOLINK/TAG literal */
    MdSlice  url;              /* LINK/IMAGE/EMBED target; WIKILINK page */
    MdSlice  alt;              /* LINK display text; IMAGE alt; WIKILINK alias */
    struct MdInline *children; /* arena-contiguous; for nested emphasis */
} MdInline;

/* Table cell holds a small inline run. */
typedef struct {
    MdInline *inlines;
    u32       inline_count;
} MdTableCell;

/* Column alignment — encoded in info-string of MD_BLOCK_TABLE as one byte
 * per column: 'l' / 'c' / 'r'. info.len == cols. */
#define MD_ALIGN_LEFT   'l'
#define MD_ALIGN_CENTER 'c'
#define MD_ALIGN_RIGHT  'r'

typedef struct MdBlock {
    u8 kind;                /* MdBlockKind */
    u8 heading_level;       /* 1..6 for HEADING; 0 otherwise */
    u8 list_ordered;        /* 0/1 for LIST_ITEM */
    u8 list_depth;          /* 0..N nesting depth (parser sets indent/2) */
    u8 task_state;          /* 0 unchecked, 1 checked (TASK_ITEM only) */
    u8 callout_kind;        /* MdCalloutKind for CALLOUT */
    u16 list_index;         /* 1-based ordinal for an ordered LIST_ITEM */
    u32 src_off;            /* TASK_ITEM: byte offset of the "[x]" state char in the parse source */

    MdSlice  info;          /* CODE: lang. CALLOUT: title text. TABLE: align bytes. */
    MdSlice  raw;           /* CODE body. FRONTMATTER body. HTML passthrough. */

    /* Inline run for PARAGRAPH/HEADING/QUOTE/LIST_ITEM body/CALLOUT title.
     * Empty for blocks whose text is in `raw` or whose content is `cells`. */
    MdInline *inlines;
    u32       inline_count;

    /* Children for LIST_ITEM, CALLOUT, QUOTE, TASK_ITEM (e.g. nested lists,
     * paragraphs inside callout body). */
    struct MdBlock *children;
    u32             child_count;

    /* TABLE only. cells[r * cols + c]. */
    MdTableCell *cells;
    u16          rows;
    u16          cols;

    /* Cross-frame layout cache (filled by md_render): the block's vertical
     * extent computed for layout generation `cached_gen`. cached_gen == 0
     * means "never measured" (fresh arena memory is zero-filled). */
    f32 cached_h;
    u32 cached_gen;
} MdBlock;

typedef struct MdDoc {
    /* The arena that owns every pointer in this struct. Borrowed — caller
     * destroys it when done with the doc. */
    void *arena;             /* Arena* (forward-decl avoidance) */
    MdBlock *blocks;
    u32      block_count;
    MdSlice  frontmatter_yaml;   /* convenience handle to MD_BLOCK_FRONTMATTER body */
    const char *base_dir;        /* malloc'd on parse OR borrowed from caller */

    /* Layout-cache generation: md_render bumps this whenever the wrap width or
     * cell metrics (w/cw/ch) change, invalidating every block's cached_h. */
    u32   layout_gen;
    f32   layout_w;
    f32   layout_cw;
    f32   layout_ch;
} MdDoc;

#endif /* UI_MARKDOWN_AST_H */
