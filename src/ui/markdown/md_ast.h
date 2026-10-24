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
    MD_BLOCK_HTML,             /* raw HTML block — shown as dimmed mono SOURCE,
                                * never interpreted (no engine, by design) */
    MD_BLOCK_MATH,             /* $$…$$ / \[…\] display math; TeX in `raw` */
    MD_BLOCK_DEF_TERM,         /* definition-list term (inlines) */
    MD_BLOCK_DEF_DESC,         /* definition-list description (inlines) */
    MD_BLOCK_EMBED,            /* ![[Note]] on its own line — transcluded */
    MD_BLOCK_DETAILS           /* <details><summary>…</summary>…</details> */
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
    MD_INLINE_WIKILINK,        /* [[Page]], [[Page|Alias]], [[Page#Heading]],
                                * [[Page#^block]] — parsed into url/anchor/alt
                                * and clickable (opens + scrolls to anchor) */
    MD_INLINE_IMAGE,           /* ![alt](url) */
    MD_INLINE_EMBED,           /* ![[file]] */
    MD_INLINE_TAG,             /* #tag */
    MD_INLINE_LINEBREAK,       /* hard break: trailing two spaces + \n */
    MD_INLINE_FOOTNOTE_REF,    /* [^id] */
    MD_INLINE_SUBSCRIPT,       /* ~sub~  (text holds the body) */
    MD_INLINE_SUPERSCRIPT,     /* ^sup^  (text holds the body) */
    MD_INLINE_MATH,            /* $inline$ — text holds raw TeX (a leaf) */
    MD_INLINE_HTML             /* inline HTML tag rendered natively (<kbd>…) */
} MdInlineKind;

/* MD_INLINE_HTML variants — which native treatment `text` gets. */
enum {
    MD_HTML_KBD = 1,           /* <kbd>…</kbd>  → key-cap chip   */
    MD_HTML_MARK,              /* <mark>…</mark> → highlight      */
    MD_HTML_BOLD,              /* <b>/<strong>                    */
    MD_HTML_ITALIC,            /* <i>/<em>                        */
    MD_HTML_UNDERLINE,         /* <u>                             */
    MD_HTML_SMALL,             /* <sup>/<sub> handled elsewhere   */
};

/* MD_INLINE_LINK flags. A reference link's destination is not known when the
 * inline is tokenized — the definition may appear later in the document — so
 * the run carries its LABEL in `url` and this flag until md_parse's resolve
 * pass swaps the real destination in. No run reaches the renderer still
 * flagged: unresolved references are demoted to plain text there. */
enum {
    MD_LINK_REF_PENDING = 1,
};

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
    MdSlice  url;              /* LINK/IMAGE/EMBED target, WITHOUT any anchor */
    MdSlice  alt;              /* LINK display text; IMAGE alt; WIKILINK alias */
    /* Obsidian target anchor, split off `url` so the path resolves as a file:
     * "Note#Heading" → url="Note", anchor="Heading", anchor_is_block=0;
     * "Note#^abc"    → url="Note", anchor="abc",     anchor_is_block=1.
     * Empty when the target carries no anchor. */
    MdSlice  anchor;
    u8       anchor_is_block;
    /* Explicit display size from `![[img.png|300]]` / `![[img.png|640x480]]`
     * / `![alt|300](img.png)`. 0 = unset (fit to the column). The `|…`
     * suffix is a SIZE only when it parses as digits or WxH — anything else
     * stays an alias, which is how the same separator serves both. */
    u16      img_w, img_h;
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
    u8 list_depth;          /* 0..N nesting depth. Derived from the CONTENT
                             * column of the enclosing item (CommonMark), not
                             * from a fixed indent step. */
    u8 task_state;          /* TASK_ITEM: 0 == unchecked ("[ ]"), otherwise the
                             * LITERAL state character — 'x', '/', '?', '-', …
                             * Obsidian gives each its own meaning, so the
                             * character is preserved rather than normalized. */
    u8 callout_kind;        /* MdCalloutKind for CALLOUT */
    u8 callout_fold;        /* CALLOUT/DETAILS: 0 not foldable, 1 "-" (starts
                             * closed), 2 "+" (starts open) */
    u8 fold_open;           /* live state: 0 = closed, 1 = open. Seeded from
                             * callout_fold on first render (fold_seen), then
                             * owned by the user's clicks. */
    u8 fold_seen;
    /* This block is not a list item but BELONGS to one: the second paragraph of
     * a loose item, a fenced block indented under it, a quote, a table.
     * `list_depth` (and `list_ordered`, for the marker width) then describe the
     * item it continues, and the renderer lays the block out under that item's
     * TEXT column instead of at the body margin, which would shatter one loose
     * list into three. Occupies padding the struct already had. */
    u8 list_cont;
    u16 list_index;         /* 1-based ordinal for an ordered LIST_ITEM */
    /* TASK_ITEM: byte offset of the "[x]" state char in the DOCUMENT source,
     * or MD_TASK_NO_SRC when this task did not come from those bytes.
     *
     * A callout/blockquote body is re-parsed out of a cleaned arena copy (the
     * '>' prefixes are stripped), so an offset into that copy is meaningless
     * against the file — and dangerously so, because a small offset lands
     * inside some unrelated task and passes the click handler's "[x]" byte
     * check. The parser therefore only records an offset it can prove points
     * into the document, and the sentinel disables toggling for the rest. */
#define MD_TASK_NO_SRC 0xFFFFFFFFu
    u32 src_off;

    MdSlice  info;          /* CODE: lang. CALLOUT: title text. TABLE: align bytes. */
    MdSlice  raw;           /* CODE body. FRONTMATTER body. HTML passthrough. */
    /* Trailing "^block-id" of a block. Obsidian uses it as a link target and
     * hides it from the rendered text — so the parser strips it from the
     * content and parks it here rather than leaving it in the prose. */
    MdSlice  block_id;

    /* Inline run for PARAGRAPH/HEADING/QUOTE/LIST_ITEM body.
     * Empty for blocks whose text is in `raw` or whose content is `cells`. */
    MdInline *inlines;
    u32       inline_count;

    /* Tokenized form of `info` for CALLOUT / DETAILS — the header label.
     * Separate from `inlines` on purpose: a callout with no child blocks keeps
     * its BODY there, so the two would collide. Without it the title would be
     * emitted as raw bytes, and "> [!note] See [[Other]] for **details**"
     * would print its own brackets and asterisks. */
    MdInline *title_inlines;
    u32       title_inline_count;

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

/* How many frontmatter list entries (tags, aliases) are kept structurally.
 * Past this the raw YAML still has them — only the parsed view truncates. */
#define MD_MAX_FM_ITEMS 16

typedef struct MdDoc {
    /* The arena that owns every pointer in this struct. Borrowed — caller
     * destroys it when done with the doc. */
    void *arena;             /* Arena* (forward-decl avoidance) */
    MdBlock *blocks;
    u32      block_count;
    MdSlice  frontmatter_yaml;   /* convenience handle to MD_BLOCK_FRONTMATTER body */

    /* The three frontmatter properties the app acts on, pulled out of the YAML
     * at parse time so nothing downstream re-scans it. Slices point INTO
     * `frontmatter_yaml`, which is kept whole and authoritative — this is a
     * convenience view over it, deliberately not a YAML object model. Both the
     * block form ("tags:\n  - a\n  - b") and the flow form ("tags: [a, b]")
     * fill these. */
    MdSlice  fm_title;
    MdSlice  fm_tags[MD_MAX_FM_ITEMS];
    MdSlice  fm_aliases[MD_MAX_FM_ITEMS];
    u8       fm_tag_count;
    u8       fm_alias_count;

    const char *base_dir;        /* malloc'd on parse OR borrowed from caller */

    /* Layout-cache generation: md_render bumps this whenever the wrap width or
     * cell metrics (w/cw/ch) change, invalidating every block's cached_h. */
    u32   layout_gen;
    u64   image_revision, embed_revision;
    f32   layout_w;
    f32   layout_cw;
    f32   layout_ch;

    /* The parse could not complete (arena exhaustion or parse_limited below).
     * Every allocation inside md_parse is individually checked, so an
     * exhausted arena never crashes — it DROPS: a block array that did not
     * fit leaves block_count at 0, an inline run that did not fit leaves the
     * paragraph blank. A large note in a tight arena can therefore come back
     * as a non-NULL document with ZERO blocks, which must not be taken (or
     * cached) as a successful parse. This flag is what separates "the note is
     * empty" from "the note did not fit"; a caller that can retry with a
     * bigger arena (md_embed) does, one that cannot says so. Counted off
     * Arena.refused, so a parse that fits reads 0 here. */
    u8    incomplete;
    /* Work, heap-vector limits or cancellation aborted this parse. No prefix AST is published and a
     * larger arena cannot help; consumers must use their source/error view. */
    u8    parse_limited;
} MdDoc;

#endif /* UI_MARKDOWN_AST_H */
