/*
 * Liu - file browser (local + SFTP)
 * Sidebar file tree with operations.
 */
#ifndef UI_FILEBROWSER_H
#define UI_FILEBROWSER_H

#include "core/types.h"
#include "core/config.h"
#include "core/memory.h"
#include "renderer/renderer.h"
#include "ui/layout.h"   /* VIEWER_* split-geometry constants */
#include "ui/syntax.h"   /* SynLang — the editor's highlighter language */
#include "ui/fb_asset_icons.h"   /* FbAssetIconKind — the file-type icon */
#include "ui/unsaved_guard.h"    /* UnsavedGuard — "refused once, repeat to do it" */
#include "ui/file_identity.h"    /* FileIdentity — which file the open text came from */
#include "ui/editor_text.h"      /* the editor's line / column / line-break arithmetic */
#include "ui/paste_report.h"     /* FbPasteOutcome — what a paste did, per item */
#include <pthread.h>     /* async graph-build worker handle */
#include <stdatomic.h>

/* Forward declarations to avoid pulling the markdown headers into every
 * translation unit that includes filebrowser.h. */
typedef struct MdDoc       MdDoc;
typedef struct MdImageCache MdImageCache;
typedef struct MdEmbedCache MdEmbedCache;
typedef struct MdLinkRect   MdLinkRect;
typedef struct MdTaskRect   MdTaskRect;
typedef struct MdFoldRect   MdFoldRect;
typedef struct MdGlyphRect  MdGlyphRect;
typedef struct MdOutlineItem MdOutlineItem;
typedef struct MdGraph      MdGraph;

/* File-browser tab content split geometry. The tab body hosts a file list on
 * the left and an optional viewer on the right; this derives the pane widths
 * from the viewer state. The renderer (render_fb_tab) and the hit-tester both
 * call this so click routing lands on exactly the pixels that were drawn —
 * keep them in lockstep by going through here rather than re-deriving the math. */
typedef struct {
    f32 list_w;   /* list-pane width (== total_w when no viewer; 0 when viewer_only) */
    f32 view_x;   /* viewer-pane left edge (valid only when has_viewer)              */
    f32 view_w;   /* viewer-pane width (0 when no viewer)                            */
} FbTabSplit;

static inline FbTabSplit fb_tab_split(f32 ox, f32 total_w, f32 dpi,
                                      bool has_viewer, bool viewer_only,
                                      f32 viewer_ratio, bool narrow_list) {
    FbTabSplit s = { total_w, 0.0f, 0.0f };
    if (!has_viewer) return s;
    if (viewer_only) {
        s.list_w = 0.0f;
        s.view_w = total_w;
        s.view_x = ox;
        return s;
    }
    f32 vmin = VIEWER_MIN_PT * dpi;
    f32 list_min = VIEWER_LIST_MIN_PT * dpi;
    /* Document views (code, markdown, graph) keep the file list a narrow
     * explorer rail so the editor fills the window — unless the user has
     * dragged the divider, which wins. */
    if (narrow_list && viewer_ratio <= 0.0f) {
        f32 lw = VIEWER_RAIL_PT * dpi;
        if (total_w - lw < vmin) lw = total_w - vmin;
        if (lw < 0.0f) lw = 0.0f;
        s.list_w = lw;
        s.view_w = total_w - lw;
        s.view_x = ox + lw;
        return s;
    }
    f32 ratio = (viewer_ratio > 0.0f) ? viewer_ratio : VIEWER_WIDTH_RATIO;
    f32 view_w = total_w * ratio;
    if (view_w < vmin)               view_w = vmin;
    if (total_w - view_w < list_min) view_w = total_w - list_min;
    if (view_w < vmin)               view_w = vmin;
    s.view_w = view_w;
    s.list_w = total_w - view_w;
    s.view_x = ox + s.list_w;
    return s;
}

/* Upper bound on clickable rects recorded per render — links AND code-block
 * Copy buttons share this budget. A 300-line README sits comfortably below it;
 * raised to 512 so a code-heavy page's Copy buttons can't crowd out navigable
 * link rects. Excess are silently dropped. */
#define FB_MD_LINK_CAP 512
/* Upper bound on clickable task-checkbox rects per render (same budget as links). */
#define FB_MD_TASK_CAP 512
#define FB_MD_FOLD_CAP 128

/* Upper bound on glyph rects collected per render for text selection. Only
 * glyphs in visible blocks are emitted, so a screenful (plus edge bleed) is
 * the worst case; surplus glyphs are dropped (selection past the cap simply
 * isn't recorded). */
#define FB_MD_GLYPH_CAP 8192

/* Upper bound on document-outline headings recorded per render. */
#define FB_MD_OUTLINE_CAP 512

#define FB_MAX_ENTRIES 1024
#define FB_MAX_PATH    1024

/* A tab in the code editor runs to the next multiple of this many columns. It
 * is the indent guides' step on purpose, so the guides beside tabbed code line
 * up with it instead of running through it. The painter, the caret and the
 * click all take it from here. */
#define FB_TAB_COLS CODE_INDENT_GUIDE_COLS

/* A single in-document find match: a byte range [start,end) into view_content. */
typedef struct { u32 start, end; } FbFindMatch;
#define FB_FIND_MAX_MATCHES 8192

typedef enum {
    FB_SOURCE_LOCAL,
    FB_SOURCE_SFTP,
} FileBrowserSource;

typedef struct {
    /* Heap-duplicated entry name. NULL until populated by fb_entry_set_name();
     * never appears in a populated entry. Frees are batched by fb_entries_reset
     * before each navigate/refresh and by fb_destroy/fb_detach_sftp. */
    char   *name;
    char   *path;          /* owned full path for a nested tree row; NULL at root */
    u64     size;
    u32     permissions;
    u64     mtime;
    bool    is_dir;
    bool    is_symlink;
    /* Has size/mtime/permissions been filled for this entry?
     *
     * A local listing takes is_dir and is_symlink from readdir's d_type and
     * does NOT stat: on a network mount a stat is a round trip, and statting
     * every entry of a large directory freezes the UI whenever the client's
     * attribute cache misses. The size column is the only consumer, so the
     * stat happens when a row is actually drawn — see fb_entry_ensure_stat.
     * SFTP listings arrive with everything already filled and set this true
     * up front. */
    bool    stat_known;
    /* A stat request for this entry is queued with the worker (see
     * fb_entry_ensure_stat): the draw must not queue it again every frame. */
    bool    stat_pending;
    i32     depth;         /* indentation level for tree view */
    bool    expanded;      /* if dir, is it expanded? */
    bool    tree_pending;  /* expanded, waiting for its child listing */
} FileEntry;

/* Set entry->name to a heap copy of src. Frees the previous name. */
void fb_entry_set_name(FileEntry *e, const char *src);

typedef enum {
    FVIEW_NONE,
    FVIEW_TEXT,
    FVIEW_MARKDOWN,
    FVIEW_CODE,
    FVIEW_IMAGE,
    FVIEW_BINARY,
    FVIEW_GRAPH,        /* knowledge-graph of the folder's .md files */
} FileViewMode;

/* Views where the open document is the content and the file list is only
 * navigation: they dock the list as a narrow explorer rail (the IDE layout) so
 * the document gets the window. Images keep the even split — they are browsed,
 * not worked in.
 *
 * ONE definition, because the split geometry is re-derived in three places
 * (render_fb_tab, the hit-tester, and the FB-tab header's button zones) and all
 * three have to agree to the pixel. */
static inline bool fb_view_wants_rail(FileViewMode m) {
    return m == FVIEW_GRAPH || m == FVIEW_MARKDOWN ||
           m == FVIEW_CODE  || m == FVIEW_TEXT;
}

/* Force-directed graph controls (Forces / Display / Filters). All force/size
 * fields are multipliers where 1.0 == the built-in default look. Persist on the
 * FileBrowser so they survive graph re-opens within a session. */
typedef struct {
    f32  center;          /* centering gravity multiplier        (0..2, def 1) */
    f32  repel;           /* node repulsion multiplier           (0..2, def 1) */
    f32  link;            /* edge spring multiplier              (0..2, def 1) */
    f32  linkdist;        /* preferred edge length multiplier    (0..2, def 1) */
    f32  node_size;       /* node radius multiplier              (0..2, def 1) */
    f32  link_thickness;  /* edge width multiplier               (0..3, def 1) */
    f32  text_fade;       /* label visibility multiplier         (0..2, def 1) */
    bool show_arrows;     /* draw arrowheads on directed links */
    bool show_orphans;    /* show notes with no links (def true) */
    bool show_tags;       /* show #tag pseudo-nodes  (def true) */
    i32  color_mode;      /* 0 = uniform (default), 1 = topic clusters */
} GraphSettings;

/* How the last fb_editor_save ended (FileBrowser.save_result). A save that
 * had nothing to write leaves the previous answer standing, so the second of
 * two calls made by one key press does not erase what the first one did. */
typedef enum {
    FB_SAVE_NONE = 0,           /* nothing has been saved from this view yet */
    FB_SAVE_OK,
    FB_SAVE_REFUSED_CHANGED,    /* the file changed on disk; a repeat overwrites */
    FB_SAVE_REFUSED_READONLY,   /* the file is read-only; a repeat overwrites */
    FB_SAVE_FAILED,             /* save_errno says why */
} FbSaveResult;

/* Sparse continuation states at line starts; at most 4096 entries per file. */
typedef struct FbSynCheckpoint {
    usize offset;
    u32 state;
    i32 line;
} FbSynCheckpoint;

typedef struct {
    bool            open;
    FileBrowserSource source;
    char            cwd[FB_MAX_PATH];
    /* The stat worker matches its results to a listing by these two: the
     * browser's identity (assigned at fb_init, never reused) and the listing
     * generation (bumped by every navigate/refresh/reset). A result for an
     * older generation names a row of a directory the user has left. */
    u64             stat_id;
    u32             stat_gen;
    FileEntry      *entries;    /* heap-allocated on navigate, freed on destroy */
    i32             entry_count;
    i32             entry_cap;
    bool            list_paused;    /* defer row replacement during drag/menu/rename */
    i32             hover_entry;    /* list row under the pointer (-1 none) —
                                     * set from MOUSE_MOVE via ui_hit_test,
                                     * drawn as a background tint (hover is
                                     * bg-colour only, same rect as selection
                                     * so every row keeps one visual shape) */
    i32             selected;       /* primary selection (last-clicked) — used
                                     * for focus ring, arrow-key target, and
                                     * as the range anchor when `selection_anchor`
                                     * is unset (navigation reset). */

    /* Multi-selection: bitmap indexed by entry index. On navigate/refresh the
     * bitmap is reset; the primary `selected` is the "anchor" for shift-range
     * operations. Cmd+click toggles, shift+click fills [anchor..idx]. */
    u8              selection_set[(FB_MAX_ENTRIES + 7) / 8];
    i32             selection_anchor;   /* -1 if no anchor yet */
    i32             selection_count;    /* cached popcount */

    i32             scroll_offset;      /* legacy integer scroll (entry index) */
    f32             scroll_offset_px;   /* smooth pixel-based scroll offset */
    f32             content_height;     /* total height of all entries in pixels */
    f32             viewport_height;    /* visible area height in pixels */
    f32             list_row_h;         /* one row's height as last drawn, px */

    /* A re-list that must not lose the user's place (fb_refresh_keep): the
     * scroll offset to put back when the root listing lands, and the path
     * whose row is selected and brought into view once it exists — which, for
     * a row inside an expanded folder, is a later listing than the root's. */
    bool            reveal_armed;
    bool            reveal_scroll_pending;
    f32             reveal_scroll_px;
    char            reveal_path[FB_MAX_PATH];
    u64             reveal_nav_generation;  /* the listing it was asked of */
    f32             width;          /* sidebar width in pixels */

    /* List scrollbar auto-hide (ui_autohide_alpha, anim.h). Stamped by the
     * render pass while the pointer is inside the sidebar, and whenever the
     * offset moved since the last frame — `sb_seen_scroll` is that previous
     * offset, which is what makes wheel, drag and keyboard row navigation all
     * count as activity without each of them having to say so. */
    f64             sb_active_t;
    f32             sb_seen_scroll;

    /* SFTP handle (opaque, from session_sftp_handle).
     * `session` is the owning Session * (also opaque to keep this header
     * decoupled from ssh_session.h). When non-NULL, libssh2 calls inside
     * filebrowser/fileops wrap themselves in session_sftp_scope_begin/end
     * to flip the underlying LIBSSH2_SESSION to blocking mode — otherwise
     * libssh2_sftp_readdir silently truncates on EAGAIN. Callers must
     * null this (or point it at the new session) when the owning tab
     * closes; session_destroy leaves a dangling pointer otherwise. */
    void           *sftp_handle;
    void           *session;

    /* File viewer */
    FileViewMode    view_mode;
    char           *view_content;    /* loaded file text (allocated view_cap bytes) */
    usize           view_size;       /* logical length (NUL at view_content[view_size]) */
    usize           view_cap;        /* allocated capacity; grows geometrically on edit */
    char            view_path[FB_MAX_PATH];
    i32             view_scroll;       /* legacy line offset — used by raw/code paths */
    f32             view_scroll_px;    /* pixel offset for smooth markdown scroll */
    f32             view_content_px;   /* total rendered pixel height (markdown), set per-frame */
    i32             view_scroll_x;     /* code/text editor: horizontal scroll, in visual columns */
    f32             hscroll_accum_px;  /* fractional horizontal trackpad pixels not yet a full column */
    f32             vscroll_accum_px;  /* fractional vertical trackpad pixels not yet a full line (ui/line_scroll.h) */

    /* Code/plain-text editor geometry of the last render (framebuffer px).
     * Consumed by fb_editor_click_caret and the horizontal caret-follow —
     * the render is the single source of truth for where column 0 of the
     * first visible line actually landed. */
    f32             code_text_x;       /* x of visual column view_scroll_x */
    f32             code_text_y;       /* y of line view_scroll */
    f32             code_cw, code_ch;  /* cell metrics used by the last render */
    i32             code_visible_lines; /* rows the last render had room for */
    i32             code_visible_cols; /* columns that fit in the text area */
    i32             code_max_vis_cols; /* longest visible line (h-scroll clamp) */
    i32             view_line_count;

    /* Syntax highlighting for the code/plain-text editor.
     *
     * `code_lang` is resolved once per open from the path; SYN_LANG_NONE means
     * "draw it flat". The scanner carries block comments / triple-quoted
     * strings / template literals across lines in a state word, so a render
     * that starts at `view_scroll` needs the state the lines ABOVE it produced.
     * Walking from byte 0 every frame would be O(file) per frame, so the last
     * walk is memoised. Sparse checkpoints also bound backwards scrolling;
     * edits retain the unchanged prefix, while document replacement drops it. */
    SynLang         code_lang;
    bool            syn_memo_valid;
    i32             syn_memo_line;
    usize           syn_memo_off;
    u32             syn_memo_state;
    FbSynCheckpoint *syn_checkpoints;
    i32             syn_checkpoint_count, syn_checkpoint_cap, syn_checkpoint_stride;

    /* Editor state */
    bool            editor_mode;      /* true = editable, false = read-only viewer */
    /* The keyboard is ELSEWHERE: the user clicked out of this document, onto
     * the terminal it is docked beside. Typing, editing keys and Find then
     * belong to the terminal, and no caret is drawn here.
     *
     * FALSE, the zero state, is "has the keyboard": a surface that never sets
     * it (a file-browser TAB has nothing else to type into) keeps the
     * keyboard. Cleared by opening a document and by a click in the viewer;
     * set by a click anywhere else and when the document comes back with its
     * tab (main.c mouse-down, app_sidebar_restore). Ask through
     * app_docked_doc_has_keyboard(). */
    bool            kbd_away;
    /* Where the editor drew its caret in the last fb_render_viewer, in
     * framebuffer pixels with the code's cell: an input method composes
     * there (ui_ime_field_caret). False when no caret was drawn. */
    bool            caret_drawn;
    f32             caret_x, caret_y, caret_cw, caret_ch;
    i32             cursor_line;      /* cursor line (0-based) */
    /* A nearby line anchor avoids rescanning the document prefix on every
     * keystroke. Offsets survive capacity growth; document edits invalidate it. */
    const char     *editor_line_cache_buffer;
    usize           editor_line_cache_offset;
    i32             editor_line_cache_line;
    bool            editor_line_cache_valid;
#ifdef FB_LINE_CACHE_TEST
    bool            editor_line_cache_reference;
#endif
    i32             cursor_col;       /* cursor column: a BYTE offset into the line, always on a character boundary */
    /* The column vertical motion is trying to keep (codepoints from the line
     * start — the unit the caret is drawn in), and the cursor position it was
     * recorded for. Down through a short line lands at its end but must come
     * back out at the same column on the next long one. It is trusted only
     * while the cursor is still where the last vertical move left it, so a
     * click, an edit or an undo discards it without anyone having to reset it. */
    i32             want_vcol, want_line, want_col;
    bool            want_valid;
    bool            modified;         /* unsaved changes */
    i32             edit_counter;     /* per-instance undo-snapshot throttle */

    /* The editor's selection (code, plain text and the Markdown Edit view). The
     * caret (cursor_line, cursor_col) is its moving end; (sel_line, sel_col) is
     * the fixed one. `sel_head_*` is where the caret stood when the selection was
     * last made or extended: a caret that someone else moved — Find, a click, an
     * undo, autocomplete — no longer matches it and the selection is gone, so no
     * code that sets the caret has to know about selections. An edit of the text
     * drops it too. Empty (anchor == caret) is no selection. */
    bool            sel_on;
    i32             sel_line, sel_col;
    i32             sel_head_line, sel_head_col;
    /* A mouse selection's unit: 1 character, 2 word (double click), 3 line
     * (triple click). The unit first picked stays selected while the drag
     * extends from it, a word or a line at a time. */
    i32             sel_unit;
    i32             sel_unit_l0, sel_unit_c0, sel_unit_l1, sel_unit_c1;
    /* The text area of the last render, framebuffer px: a drag past its top or
     * bottom edge scrolls (fb_editor_drag). */
    f32             ed_area_y, ed_area_h;

    /* The line ending the file had when it was read (what dominates in it:
     * editor_detect_crlf). Enter and a paste write THIS one; the status bar
     * names it. Which bytes end one particular line is asked of the line. */
    bool            view_crlf;
    /* Moves with every change to view_content — an edit, an undo, a reload,
     * another document. Whatever is computed FROM the text keeps the value it
     * saw and knows it is stale when the two differ: the Find match list
     * (find_gen), the encoding the status bar names (view_enc_for). */
    u32             view_edit_gen;
    u8              view_enc;         /* EditorEncoding, as of view_enc_for */
    u32             view_enc_for;     /* view_edit_gen + 1 it was read at; 0 = never */

    /* The file view_content came from, as it was when it was read or last
     * written (ui/file_identity.h). A save, and a task-checkbox click in a
     * note, ask the disk whether that is still the file there before they
     * write over it. Not valid: nothing recorded, nothing is refused. */
    FileIdentity    view_disk;
    /* The action that was refused because it would have thrown the edits
     * away, waiting for its repeat (ui/unsaved_guard.h). */
    UnsavedGuard    guard;
    /* What the last refused or failed action has to say — "x has unsaved
     * changes — …", "Not saved: …". The browser does not draw it: whoever
     * holds the browser shows it, taking each one once with fb_notice_take().
     * `notice_at` is platform_time_sec() when it was set. */
    char            notice[192];
    f64             notice_at;
    u32             notice_seq, notice_taken;
    bool            notice_error;     /* refused / failed, as against "reloaded" */
    u8              save_result;      /* FbSaveResult of the last save */
    int             save_errno;       /* errno of a FB_SAVE_FAILED */

    /* Journaled undo stack.
     *
     * Each frame stores the splice that transforms state[i] → state[i+1]
     * (old bytes at offset replaced by new bytes), never a buffer copy. The
     * frame is built from the edits THEMSELVES: every mutation of view_content
     * goes through editor_apply(), which records the bytes it is about to
     * remove and widens the pending span, and fb_editor_push_undo turns the
     * pending span into a frame by copying the span's current bytes out of
     * the document. Nothing is diffed and nothing is mirrored: the undo record
     * of a keystroke is the keystroke.
     *
     * - `undo_frames[i]` is the forward splice from state[i] → state[i+1].
     * - `undo_pos` is the user-visible position: view_content == state[undo_pos]
     *   plus whatever `undo_pending` holds (edits not yet committed to a frame).
     * - `undo_base_cursor_*` is the cursor at state[0]; when the stack is full
     *   the oldest frame is dropped and state[0] moves forward.
     * - `undo_pending` is one covering span: the bytes the committed state had
     *   there (`old_bytes`) and the span's current length. Every byte outside
     *   the span is identical in both states, which is what lets a new edit
     *   anywhere widen it with bytes read straight from the document. */
    struct FbUndoFrame {
        usize offset;       /* splice position in state[i] */
        usize old_len;      /* bytes removed at offset (may be 0) */
        usize new_len;      /* bytes inserted at offset (may be 0) */
        char *old_bytes;    /* owned; NULL iff old_len == 0 */
        char *new_bytes;    /* owned; NULL iff new_len == 0 */
        i32   cursor_line;  /* cursor AT state[i+1] */
        i32   cursor_col;
    } undo_frames[32];
    struct FbUndoPending {
        bool  active;
        usize start;        /* span start — the same offset in both states */
        usize cur_len;      /* span length in the CURRENT document */
        char *old_bytes;    /* the committed state's bytes over the span */
        usize old_len, old_cap;
    } undo_pending;
    i32             undo_base_cursor_line;
    i32             undo_base_cursor_col;
    i32             undo_count;           /* number of valid frames */
    i32             undo_pos;             /* current state index: 0..undo_count */

    /* Markdown rendering state.
     *
     * On open of a .md/.markdown file, `md_arena` is created and md_parse
     * fills `md_doc` with the parsed AST. `md_images` caches inline images
     * (RGBA via stb_image) keyed by absolute path; freed on viewer close.
     * Source bytes (view_content) outlive md_doc — slices in the AST point
     * directly into view_content. */
    Arena         md_arena;
    MdDoc        *md_doc;
    MdImageCache *md_images;
    /* Documents pulled in by `![[Note]]` transclusions. Same lifetime rule
     * as md_images: created with the viewer, dropped when it closes. */
    MdEmbedCache *md_embeds;
    bool          md_raw_mode;     /* false = rendered, true = raw source */
    bool          md_source_dirty; /* source changed since the current AST */

    /* Click-targets recorded by md_render each frame. Rects sit in viewport
     * coordinates (framebuffer pixels). Cleared and refilled per frame; safe
     * to consult immediately after fb_render_viewer returns. URL pointers
     * borrow from the parse arena, valid until the doc is destroyed. */
    MdLinkRect   *md_link_rects;
    u32           md_link_count;
    /* Copy-confirmation flash for a fenced block's Copy button. `src` is the
     * block's raw text pointer — the same one the click rect carries, so the
     * click handler stamps it without needing a block id. It is only ever
     * COMPARED, never dereferenced, which is what makes it safe to leave
     * pointing into a parse arena the next navigation frees: the worst a stale
     * address can do is match a new block that happens to land on it, and only
     * inside the flash's own second-and-a-half. Read by fb_md_copy_flash(); a
     * live flash must keep the loop painting, which is what
     * app_md_copy_flash_active() is for. */
    const u8     *md_copy_flash_src;
    f64           md_copy_flash_at;
    /* Clickable task-checkbox rects, same per-frame lifecycle as md_link_rects. */
    MdTaskRect   *md_task_rects;
    MdFoldRect   *md_fold_rects;
    u32           md_fold_count;
    u32           md_task_count;

    /* Markdown rendered-view text selection. The renderer fills
     * `md_glyph_rects` (reading order, framebuffer px) every frame. The
     * selection endpoints are stored in CONTENT space (screen minus the
     * cached content origin, plus scroll) so they survive scrolling and
     * reflow. `md_origin_x/y` cache the last render's content origin so the
     * key/mouse handlers can map framebuffer coords without re-deriving the
     * viewer layout. */
    MdGlyphRect  *md_glyph_rects;
    u32           md_glyph_count;
    bool          md_sel_active;
    f32           md_sel_ax, md_sel_ay;     /* anchor (content space) */
    f32           md_sel_hx, md_sel_hy;     /* head   (content space) */
    f32           md_origin_x, md_origin_y; /* content origin, last render */
    /* Height of the viewport the markdown was last laid out into. The scroll
     * clamp needs it: the furthest useful scroll is (content - viewport). */
    f32           md_table_scroll_x, md_table_overflow;
    f32           md_view_h;
    f32           live_width, live_skip_px, live_cursor_x;
    struct {
        const char *buffer;
        u32 generation;
        i32 line, col;
        f32 width, height, cw, ch, dpi, scroll;
        bool valid;
    } live_layout_cache;
    bool          live_follow;
    f32           md_read_saved_scroll;
    bool          md_restore_read_scroll;
    f32           live_cw, live_ch, live_dpi; /* live-preview metrics, last render (click→cursor) */
    /* Scroll value the glyph rects were captured at. Used by
     * md_sel_index_at to convert rects back to doc space without
     * accidentally applying the current scroll twice. Without this,
     * a scroll-mid-drag offsets every endpoint mapping by the scroll
     * delta and the selection grabs the wrong glyphs for one frame. */
    f32           md_glyph_scroll_px;
    /* Cmd+C on a Markdown selection is answered on the NEXT frame: that
     * frame renders with collect_all and copies from a whole-document glyph
     * list, then rebuilds the viewport-only list the highlight reads. */
    bool          md_copy_pending;

    /* Document outline (headings) collected each markdown render, in document
     * order. Drives the command-palette "Go to Heading" quick-nav. */
    MdOutlineItem *md_outline;
    u32            md_outline_count;

    /* Pending "[[Note#Heading]]" jump. Set when a wikilink with an anchor is
     * followed; the block index is resolved from the anchor at click time, and
     * the renderer reports that block's document-space y on the next frame
     * (md_seek_y), which the viewer then scrolls to and disarms.
     * MD_SEEK_NONE means "no jump pending". */
    u32            md_seek_block;
    f32            md_seek_y;

    /* In-document Find / Replace (markdown rendered + raw, and code/text).
     * Matches are byte ranges into view_content — the single source of truth
     * that maps to glyph src_off in read mode and to raw line offsets in edit
     * mode. */
    bool          find_active;
    bool          find_replace_mode;   /* show the replace row + enable replace */
    bool          find_case;           /* case-sensitive */
    bool          find_regex;          /* POSIX extended regex */
    bool          find_focus_replace;  /* replace field has focus (Tab toggles) */
    bool          find_regex_error;    /* last recompute failed to compile */
    char          find_query[256];     u32 find_query_len;
    char          find_replace[256];   u32 find_replace_len;
    FbFindMatch  *find_matches;
    u32           find_match_count, find_match_cap;
    i32           find_current;        /* index of the focused match, or -1 */
    /* The view_edit_gen the match list was computed at. The list is byte
     * ranges: after any edit they name other bytes, and a Replace that
     * spliced at them would write into the wrong place. Every reader of
     * the list rescans first when this is behind (fb_find_sync). */
    u32           find_gen;
    bool          find_truncated;      /* the list stopped at FB_FIND_MAX_MATCHES */

    /* Wikilink / tag autocomplete (markdown edit). The app layer fills
     * `ac_items` from the vault file list (wikilinks) or in-buffer tags and
     * drives the popup; the renderer anchors it under the caret. */
    bool          ac_active;
    u8            ac_kind;             /* 0 = wikilink, 1 = tag */
    usize         ac_anchor;           /* byte offset just after "[[" or "#" */
    char          ac_query[128];
    i32           ac_sel;
    char        (*ac_items)[160];
    u32           ac_count, ac_cap;
    f32           caret_px_x, caret_px_y, caret_px_h;  /* last caret (popup anchor) */

    /* Image viewer state (used when view_mode == FVIEW_IMAGE). RGBA pixels
     * come from stbi_load and are freed on close (or on next image open).
     * `view_image_key` is the stable cache key handed to the renderer's
     * texture cache — it's the FileBrowser pointer XOR a generation
     * counter so back-to-back opens of different images don't collide.
     *
     * Animated GIFs go through stbi_load_gif_from_memory which returns a
     * single contiguous buffer of (frame_count) frames stacked (each
     * w*h*4 bytes) plus a malloc'd `frame_delays` array (in 1/100 s units,
     * GIF's native quantum). For static images, frame_count == 1 and
     * `frame_delays` is NULL. The render advances `frame_index` based on
     * elapsed wall time vs. `frame_anchor_time + sum(delays[0..idx])`. */
    u8           *view_image_rgba;
    i32           view_image_w;
    i32           view_image_h;
    u64           view_image_gen;
    i32           view_image_frame_count;
    i32          *view_image_frame_delays;   /* malloc'd by stb */
    i32           view_image_frame_index;
    f64           view_image_frame_anchor;   /* platform_time_sec at frame[0] start */
    f64           view_image_next_frame_at;  /* updated each render — used by main loop's timeout */
    bool          view_image_is_animated;
    /* The picture is a drawn SVG (ui/svg_raster.h), and its ink is mostly dark:
     * it is backed by a light checkerboard, where every other image keeps the
     * dark one. */
    bool          view_image_svg;
    bool          view_image_ink_dark;
    /* The next fb_open_file shows an SVG as its source (an open that asked to
     * edit) instead of drawn. Set and cleared around the call, never kept. */
    bool          open_svg_source;
    /* The title bar's switch as last drawn: x, y, w, h in framebuffer pixels,
     * zeros when there is none. */
    f32           viewer_toggle[4];
    /* User-controlled zoom (1.0 = fit to viewer area). Reset to 1.0 on
     * every open so a small image doesn't carry a previous file's zoom. */
    f32           view_image_zoom;
    f32           view_image_pan_x;
    f32           view_image_pan_y;
    /* Still/GIF disk read + decode worker.
     *
     * The job is a SEPARATELY OWNED block, not fields in here, and it is
     * refcounted between this FileBrowser and its worker. That is what lets
     * fb_destroy drop its reference and return instead of joining a decode in
     * progress — the limits allow a 128 MiB still or a 64 MiB animation, so a
     * join would be an unbounded freeze on a tab or window close. A worker
     * whose FileBrowser is gone finds itself holding the last reference and
     * frees the job, so it can never write into freed state.
     *
     * Result fields are worker-owned until FbImageJob.done is published;
     * fb_image_decode_reap moves them into the live viewer state. */
    struct FbImageJob *image_job;

    /* Remote directory listing, off the main thread for the same reason: a
     * libssh2 opendir + readdir walk is network round trips, which on the
     * main thread would freeze the window for as long as the server takes.
     * Owned jointly with its worker, like the image job. `nav_generation` is
     * what makes a late result safe to throw away: a listing installs only
     * if the browser is still where it was asked from. */
    struct FbSftpListJob *sftp_list_job;
    u64                   nav_generation;
    /* Local listing, same ownership model. opendir/readdir/NFC/sort run on a
     * worker; fb_navigate returns without waiting and the rows arrive via
     * fb_poll_async. A file asked for by name while the listing is still in
     * flight (fb_open_md_path) is opened when it lands. */
    struct FbLocalListJob *local_list_job;
    struct FbTreeRestore *tree_restore; /* expanded paths retained across refresh */
    /* Markdown opens (read + parse) and reparses above the small synchronous
     * bound run on a worker against its own copy of the source; the AST points into
     * md_src (NOT view_content) for as long as it lives. NULL md_src means
     * the AST borrows view_content, the synchronous small-document case. */
    struct FbMdParseJob   *md_parse_job;
    u8                    *md_src;
    char                   pending_open_name[256];
    bool                   pending_open_keep_graph;
    bool                   pending_open_edit;
    char                   pending_open_anchor[256];
    bool                   pending_open_anchor_is_block;

    /* Graph view (FVIEW_GRAPH): force-directed knowledge graph of the
     * folder's .md files, CPU-rasterized + drawn via the image path. */
    MdGraph      *graph;
    /* Async build: md_graph_build does recursive disk I/O + parsing, which on a
     * slow / network volume (plus the macOS file-access prompt) would freeze the
     * main thread. So the scan runs on a worker, and the view shows a spinner
     * until fb_graph_poll() adopts the result.
     *
     * The scan is a heap JOB and this is the browser's handle to it. Its
     * directory, its cancel/done flags and its result live in the job; the
     * worker is given the job and never this struct — a FileBrowser is a value
     * that MOVES (stowed into a tab, carried to another window), and a worker
     * holding its address would deliver one tab's graph to whatever sits there
     * next. NULL when no scan is in flight or waiting. */
    struct FbGraphJob *graph_job;
    f32           graph_pan_x, graph_pan_y;  /* screen-space pan offset */
    f32           graph_zoom;                /* world->screen scale */
    bool          graph_fitted;              /* auto-fit done on first render */
    f64           graph_intro_start;         /* entrance animation start (0 = none) */
    u64           graph_raster_gen;          /* bumped to re-upload while animating */
    i32           graph_hover;               /* hovered node, -1 none */
    /* Hover-focus animation: when a node is hovered its connected web lights up
     * while the rest dims. graph_focus_t eases 0→1 on hover-in and 1→0 on
     * hover-out so the emphasis fades in/out instead of popping. graph_focus_node
     * holds the node the animation is built around — it outlives graph_hover
     * during the fade-out so the "who was connected" set stays known while the
     * highlight recedes. */
    f32           graph_focus_t;             /* 0 = no focus, 1 = full focus */
    i32           graph_focus_node;          /* node the focus anim centres on, -1 none */
    i32           graph_drag_node;           /* node being dragged, -1 none */
    bool          graph_panning;
    f32           graph_press_x, graph_press_y;  /* press origin (for click vs drag) */
    f32           graph_last_x, graph_last_y;     /* last move pos */
    bool          graph_moved;               /* moved past click threshold */
    bool          reset_split_narrow;          /* on entry: snap the tab's list/graph
                                               * divider back to a narrow sidebar */
    char          graph_root[FB_MAX_PATH];    /* folder the graph was first opened
                                               * on; the stable root the folder
                                               * filter enumerates from */
    bool          md_only_entries;            /* entry list narrowed to .md files
                                               * for graph mode; restored to the
                                               * full listing on viewer close */
    bool          graph_return;               /* a note was opened from the graph;
                                               * the graph is kept alive so closing
                                               * the note returns to it */
    f64           graph_next_frame_at;       /* main-loop redraw scheduler */
    f64           graph_step_prev_t;         /* wall clock of the last sim step
                                              * (md_graph_step gets REAL elapsed
                                              * time, not one tick per frame) */
    f32           graph_view_x, graph_view_y;   /* framebuffer rect last drawn into */
    f32           graph_view_w, graph_view_h;
    /* Label declutter scratch (reused per frame, grown lazily, freed in
     * fb_destroy). Split panes each own a FileBrowser, so this must be
     * per-instance — never a function-local static. */
    f32           graph_fit_zoom;            /* zoom at last auto-fit (relative label fade) */
    bool          graph_user_zoomed;         /* user panned/zoomed → stop auto re-fit */
    u8           *graph_lbl_grid;            /* screen-space occupancy bitmap (cols*rows) */
    i32           graph_lbl_grid_cap;        /* allocated cells */
    u64          *graph_lbl_key;             /* candidates packed (priority<<20 | index) */
    i32           graph_lbl_cap;             /* allocated candidate slots */
    u8           *graph_nbr_set;             /* hovered node's neighbour bitset (per node) */
    i32           graph_nbr_cap;             /* allocated bytes */

    /* Force-directed graph controls (Forces / Display / Filters). */
    GraphSettings gset;
    bool          gset_inited;               /* defaults applied yet? */
    bool          gset_dirty;                /* gset changed → push to sim next frame */
    bool          graph_settings_open;       /* the controls panel is expanded */
    i32           graph_drag_slider;         /* slider being dragged, -1 = none */
    f32           graph_render_dpi;          /* dpi from last render (panel hit-test) */
    bool          graph_owns_tab;            /* this FB tab was created to host the
                                              * graph; closing the graph closes it */
    char          graph_vault_root[FB_MAX_PATH]; /* the notes Vault's path, stamped
                                              * when the Vault graph is opened.
                                              *
                                              * The tab is named "Graph View"
                                              * while graph_root EQUALS this, and
                                              * after the folder otherwise. It is
                                              * a comparison rather than a flag on
                                              * purpose: a flag saying "this was
                                              * opened as the Vault graph" goes on
                                              * lying the moment the graph moves
                                              * — the up button re-roots it at the
                                              * vault's PARENT, the list rail
                                              * narrows it to a subfolder, and Esc
                                              * closes it and leaves the tab free
                                              * to graph something else entirely.
                                              * The name has to describe what is
                                              * on screen, so it is derived from
                                              * what is on screen. Never cleared;
                                              * the comparison does the work. */
} FileBrowser;

/* Init / lifecycle */
/* True when a viewer image decode has finished and is waiting to be installed
 * by the next fb_render_viewer. The main loop uses it to ask for that frame. */
bool fb_image_decode_ready(const FileBrowser *fb);

/* Install whatever the browser's workers have finished — the viewer image and
 * the remote directory listing. Cheap when idle; call once per frame per
 * browser. fb_render_viewer calls it, so the docked viewer is covered; a
 * sidebar-only browser needs the main loop to call it. */
void fb_poll_async(FileBrowser *fb);

/* True when a worker result is waiting to be installed — the main loop's
 * redraw gate asks this so the frame that installs it actually happens. */
bool fb_async_pending(const FileBrowser *fb);

/* True while a remote listing is in flight. The entry list is empty until it
 * lands, which is the visible cost of not freezing the window instead. */
bool fb_sftp_listing_active(const FileBrowser *fb);
/* True while a Markdown open or edited-buffer parse is in flight. */
bool fb_md_parse_active(const FileBrowser *fb);
/* True while any local text/code/Markdown source is being read off-thread. */
bool fb_file_open_active(const FileBrowser *fb);
/* Tests: the size from which a Markdown parse moves off the main thread. */
void fb_md_set_async_min(usize bytes);
/* Tests: delay before opening a Markdown file; copied into each new job. */
void fb_md_set_read_delay(i32 ms);
/* True while a local listing is in flight. */
bool fb_local_listing_active(const FileBrowser *fb);
/* Local-listing worker threads alive process-wide. */
i32  fb_local_list_live(void);
/* Tests: the navigate wait budget and a delay before opendir that stands in
 * for a slow mount (LIU_FB_LIST_WAIT_MS / LIU_FB_LIST_DELAY_MS otherwise). */
void fb_local_list_set_timing(i32 wait_ms, i32 delay_ms);

void fb_init(FileBrowser *fb);
void fb_destroy(FileBrowser *fb);
/* Free the names of every populated entry and reset entry_count to 0. */
void fb_entries_reset(FileBrowser *fb);

/* Multi-selection helpers. Bitmap operations are O(1); selection_count is
 * maintained incrementally so callers can cheaply query "is anything selected?".
 * All of these bounds-check `idx` against entry_count — safe on stale input. */
void fb_sel_clear(FileBrowser *fb);
void fb_sel_set(FileBrowser *fb, i32 idx, bool on);
void fb_sel_toggle(FileBrowser *fb, i32 idx);
void fb_sel_range(FileBrowser *fb, i32 a, i32 b);      /* inclusive [a..b] */
bool fb_sel_has(const FileBrowser *fb, i32 idx);
i32  fb_sel_collect(const FileBrowser *fb, i32 *out, i32 cap);  /* returns count */

/* Detach the filebrowser from an SFTP session — called when the owning
 * Session is about to be destroyed or reconnected so the handle doesn't
 * dangle. Flips source back to FB_SOURCE_LOCAL with an empty entry list. */
void fb_detach_sftp(FileBrowser *fb);

/* Navigate to directory (local or SFTP) */
/* Fill one entry's size/mtime/permissions if the listing did not. A local
 * listing deliberately does not stat (see FileEntry.stat_known); the row draw
 * calls this for the entries actually on screen. */
void fb_entry_ensure_stat(FileBrowser *fb, FileEntry *e);
/* Adopt the stat results the worker has finished for THIS browser's current
 * listing. Called at the top of the list draw; returns true when a row's
 * figures changed (the frame should be painted). */
bool fb_stat_poll(FileBrowser *fb);
/* Diagnostics: requests queued / results delivered / dropped process-wide. */
void fb_stat_counters(u64 *queued, u64 *done, u64 *dropped);
/* Stop the worker and free its queues. Idempotent; process shutdown. */
void fb_stat_shutdown(void);

bool fb_navigate(FileBrowser *fb, const char *path);
bool fb_navigate_sftp(FileBrowser *fb, void *sftp_handle, const char *path);
void fb_go_up(FileBrowser *fb);
void fb_refresh(FileBrowser *fb);
/* fb_refresh() for a listing the user is LOOKING at: a plain refresh snaps the
 * list to the top with nothing selected, which leaves the file just renamed or
 * created somewhere off screen and unmarked. This one keeps the scroll offset
 * and selects `select_path` (NULL: whatever was selected) once its row is
 * listed. */
void fb_refresh_keep(FileBrowser *fb, const char *select_path);

/* File operations */
bool fb_open_file(FileBrowser *fb, i32 index);
/* Toggle children inline without changing cwd or the open document. */
bool fb_toggle_directory(FileBrowser *fb, i32 index);
/* Selected folder, or the selected file's parent; otherwise the tree root. */
void fb_operation_dir(const FileBrowser *fb, char *out, usize cap);
/* Returns false when the first close attempt keeps a document with unsaved
 * edits open; `fb->notice` explains why. A repeated attempt within the guard
 * interval closes it. Callers that continue as if it closed must check the
 * result. */
bool fb_close_viewer(FileBrowser *fb);
/* Close variant: keep_graph=true preserves fb->graph for the paired
 * fb_reenter_graph (return-to-graph); every other caller uses fb_close_viewer. */
bool fb_close_viewer_ex(FileBrowser *fb, bool keep_graph);

/* ---- Unsaved edits, the file on disk, and what the browser has to say ----
 *
 * Opening another document, closing the viewer and reloading are refused the
 * first time when the open one has unsaved edits, and go through when
 * repeated. The places that free the text ask on their own; these are for
 * whoever holds the browser. */

/* A text, code or Markdown document is open and has edits that are not on
 * disk. What to ask before quitting or closing the tab that holds it. */
bool fb_editor_dirty(const FileBrowser *fb);
/* The repeat rule, for an action the caller is about to take itself (quit,
 * close the tab: UNSAVED_QUIT). TRUE = go ahead — nothing is unsaved, or this
 * is the confirming repeat. FALSE = refused, and the notice says why. */
bool fb_unsaved_confirm(FileBrowser *fb, UnsavedAction action);
/* Discarding clears the editor's unsaved state so a later close or open
 * proceeds without a prompt. It does not change the file on disk. */
void fb_editor_discard(FileBrowser *fb);
/* Hand out the notice once: copies the newest one nobody has taken yet into
 * `out` and returns true; false when there is nothing new. `*is_error` (may
 * be NULL) is false for information ("… reloaded"). */
bool fb_notice_take(FileBrowser *fb, char *out, usize cap, bool *is_error);
/* Leave a notice of the caller's own on the browser (fb_find.c does, and a
 * file operation may): the same one line, shown the same way. */
void fb_notice_set(FileBrowser *fb, bool error, const char *text);

/* Returns true when a stat detects a change in the path's device, inode,
 * size, or modification time from the view's recorded file state. Returns
 * false when no document is open, the file is missing, or the stat fails. */
bool fb_view_changed_on_disk(FileBrowser *fb);
/* Reloads `view_path` while preserving the view mode, scroll position, and
 * caret line. Returns false with a notice if unsaved edits would be
 * overwritten; otherwise returns true after loading the file. */
bool fb_view_reload(FileBrowser *fb);
/* A file or folder was renamed or moved from `old_path` to `new_path`: an
 * open document that was it, or was inside it, follows. fb_rename calls this
 * for its own browser; another browser showing the same file has to be told. */
void fb_view_path_moved(FileBrowser *fb, const char *old_path, const char *new_path);
/* `path` was deleted or moved to the Trash: an open document that was it, or
 * was inside it, stays open and becomes unsaved — the text in the viewer is
 * now the only copy, and a save writes the file again. */
void fb_view_path_gone(FileBrowser *fb, const char *path);

/* Graph view: build a knowledge graph of the .md files under fb->cwd and
 * enter FVIEW_GRAPH (or exit it if already active). Returns true if the graph
 * view is active afterwards. */
bool fb_toggle_graph(FileBrowser *fb);
/* Toggle the graph rooted at `root` (e.g. the notes Vault) instead of fb->cwd. */
bool fb_toggle_graph_root(FileBrowser *fb, const char *root);
/* Rebuild the graph rooted at `dir` (recursively), staying in graph mode — the
 * folder filter. Returns true if `dir` had any .md files. */
bool fb_graph_rescope(FileBrowser *fb, const char *dir);
bool fb_graph_active(const FileBrowser *fb);
/* A scan is running, or waiting for a worker: the view shows its spinner. */
bool fb_graph_scanning(const FileBrowser *fb);
/* Adopt a finished scan into `fb` (main thread). True when the browser changed
 * — a graph arrived, or the scan ended with none. Called by the graph view on
 * every frame and by the completion pump for browsers that are not on screen. */
bool fb_graph_poll(FileBrowser *fb);
/* Scan worker threads alive right now, and how many may run at once. A scan
 * asked for past the cap waits its turn (fb_graph_poll starts it). */
i32  fb_graph_jobs_live(void);
i32  fb_graph_jobs_cap(void);
/* Returning from a note opened off the graph back to the (preserved) graph. */
bool fb_graph_can_return(const FileBrowser *fb);
void fb_reenter_graph(FileBrowser *fb);

/* Graph interaction. (rel_x, rel_y) are coords within the viewer content rect
 * (w x h) that fb_render_viewer was last drawn into; main.c supplies them.
 * fb_graph_release returns the .md path to OPEN when the gesture was a click
 * (no drag) on a node, else NULL. */
void        fb_graph_press(FileBrowser *fb, f32 rel_x, f32 rel_y, f32 w, f32 h);
void        fb_graph_move(FileBrowser *fb, f32 rel_x, f32 rel_y, f32 w, f32 h);
const char *fb_graph_release(FileBrowser *fb, f32 rel_x, f32 rel_y, f32 w, f32 h);
void        fb_graph_scroll(FileBrowser *fb, f32 dy, bool precise,
                            f32 rel_x, f32 rel_y, f32 w, f32 h);
void        fb_graph_hover(FileBrowser *fb, f32 rel_x, f32 rel_y, f32 w, f32 h);
/* True while a graph pan / node-drag gesture is in progress. */
bool        fb_graph_gesture(const FileBrowser *fb);

/* Graph controls panel (Forces / Display / Filters). Coords are ABSOLUTE
 * framebuffer pixels — the panel positions itself inside graph_view_*. _hit
 * reports whether a point lands on the gear button or the open panel (so the
 * caller suppresses node-hit / pan); _press consumes a click on the gear, a
 * slider, or a checkbox and returns true; _drag updates the held slider. */
bool        fb_graph_panel_hit(const FileBrowser *fb, f32 sx, f32 sy);
bool        fb_graph_panel_press(FileBrowser *fb, f32 sx, f32 sy);
void        fb_graph_panel_drag(FileBrowser *fb, f32 sx, f32 sy);
void        fb_graph_panel_release(FileBrowser *fb);
/* Request an arbitrary .md by absolute path (navigates to its folder, exits
 * graph mode). True means accepted; listing/read/parse land via fb_poll_async. */
bool        fb_open_md_path(FileBrowser *fb, const char *abs_path);
/* Request a local file by path, preserving edit intent through asynchronous
 * listing and source loading. True means accepted; poll for the loaded view. */
bool        fb_open_path(FileBrowser *fb, const char *path, bool edit);

/* Rendering. `bg` is the colour of the surface the list is drawn ON (its
 * header tint and scrollbar track derive from it); the widget paints no fill
 * of its own, the caller owns the surface. */
void fb_render_sidebar(FileBrowser *fb, Renderer *r, f32 x, f32 y, f32 w, f32 h,
                       const Color *bg, const Color *fg, const Color *sel,
                       const Color *dim, const Color *accent, f32 dpi,
                       f32 hover_x, f32 hover_y);
/* hover_x/hover_y are framebuffer pointer coords, used for the title bar's
 * clickable chrome (close, Read/Edit). Pass -1,-1 to render with no hover. */
void fb_render_viewer(FileBrowser *fb, Renderer *r, f32 x, f32 y, f32 w, f32 h,
                      f32 dpi, const Theme *theme, f32 opacity,
                      f32 hover_x, f32 hover_y);

/* Input */
bool fb_handle_click(FileBrowser *fb, f32 rel_x, f32 rel_y, f32 w, f32 h, f32 dpi);
void fb_handle_scroll(FileBrowser *fb, f32 dy, bool precise);
/* The height, in framebuffer pixels, of one line of the view that scrolls by
 * line (the code editor's cell, the Markdown live editor's body line) as the
 * last render drew it; 0 for any other view. A wheel step is a multiple of it. */
f32 fb_view_row_px(const FileBrowser *fb);
/* One wheel event for such a view (code, text, the Markdown live editor): the
 * pixels of a trackpad are carried until they make a line, a wheel notch is
 * whole lines, and the first visible line stays inside the document.
 * `fallback_row_px` is the step before the view has been drawn once. */
void fb_scroll_lines(FileBrowser *fb, f32 dy, bool precise, f32 fallback_row_px);

/* Markdown viewer link hit-test. (vx, vy) are framebuffer pixel coords. If a
 * link was rendered under the point, copies its URL into `out` (nul-terminated,
 * truncated to out_cap-1), and returns true. Rects must be populated by a
 * prior fb_render_viewer for the same frame (or the most recent one when
 * polling on mouse-move). Out remains untouched on miss. */
bool fb_md_hit_link(const FileBrowser *fb, f32 vx, f32 vy,
                    char *out, usize out_cap, u8 *out_kind);

/* Same hit test, but also reports the link's "#Heading" / "#^block-id" anchor
 * (already split off the path by the parser — no string re-parsing here).
 * `out_anchor` receives the fragment without its '#'/'^', and
 * `*out_anchor_is_block` distinguishes the two. Pass NULLs to ignore. */
bool fb_md_hit_link_anchored(const FileBrowser *fb, f32 vx, f32 vy,
                             char *out, usize out_cap, u8 *out_kind,
                             char *out_anchor, usize out_anchor_cap,
                             bool *out_anchor_is_block);

/* Arm a scroll-to-anchor for the currently open document. Resolves the anchor
 * to a block via md_embed_section and lets the next render report its y.
 * No-op when the anchor names nothing. */
void fb_md_seek_anchor(FileBrowser *fb, const char *anchor, bool is_block);

/* Click a rendered task-list checkbox to toggle "[ ]"<->"[x]" in the source file
 * (read-mode only). Reparses + persists on a hit. Returns true if a box was hit. */
bool fb_md_toggle_task_at(FileBrowser *fb, f32 vx, f32 vy);

/* Hit-test a foldable block's header (callout with a -/+ marker, or a
 * <details>) and flip it open/closed. Pure view state: nothing is written
 * back to the file, because the marker in the source says where it STARTS,
 * not where the reader last left it. */
bool fb_md_toggle_fold_at(FileBrowser *fb, f32 vx, f32 vy);

/* Rendered-markdown text selection. Coordinates are framebuffer px (same
 * space as fb_md_hit_link). `begin` anchors a drag at the point (collapsing
 * any prior selection); `update` extends it to the point; `clear` drops it.
 * `active` reports whether a non-empty selection exists; `copy` writes the
 * selected text to the clipboard (newline at each visual line break) and
 * returns true if anything was copied. All are no-ops outside markdown
 * render mode. */
void fb_md_selection_begin(FileBrowser *fb, f32 mx, f32 my);
void fb_md_selection_update(FileBrowser *fb, f32 mx, f32 my);
void fb_md_selection_clear(FileBrowser *fb);
bool fb_md_selection_active(const FileBrowser *fb);
bool fb_md_selection_copy(FileBrowser *fb);
/* The selection's text as far as the glyphs ON SCREEN go (the copy takes the whole
 * document's, on its own frame): malloc'd, or NULL. For the driver and the tests. */
char *fb_md_selection_text_visible(FileBrowser *fb);
/* Select the whole rendered note (Cmd+A): the copy runs its own whole-document
 * pass, so it is the document's text, not what is on screen. */
void fb_md_select_all(FileBrowser *fb);
/* Select the word (double click) or the logical line — a wrapped paragraph is
 * one (triple click) under the point. FALSE when no glyph is near it. */
bool fb_md_select_word_at(FileBrowser *fb, f32 mx, f32 my);
bool fb_md_select_line_at(FileBrowser *fb, f32 mx, f32 my);

/* Read/Edit (preview/write) toggle for a markdown viewer.
 * Entering Edit makes the raw buffer editable; leaving it re-parses so the
 * rendered preview reflects the edits. fb_md_edit_active reports write mode. */
void fb_md_toggle_edit(FileBrowser *fb);
bool fb_md_edit_active(const FileBrowser *fb);

/* The viewer's title-bar switch between two views of ONE document: a Markdown
 * note is Read | Edit, an SVG is Image | Code (drawn, or its source in the
 * editor). The kind says whether there is a switch and which; fb_viewer_toggle
 * flips it. An SVG opens drawn; the switch to its source, and back, asks before
 * it throws unsaved edits away like any other open. */
typedef enum { FB_TOGGLE_NONE, FB_TOGGLE_MARKDOWN, FB_TOGGLE_SVG } FbViewerToggle;
FbViewerToggle fb_viewer_toggle_kind(const FileBrowser *fb);
void fb_viewer_toggle(FileBrowser *fb);
bool fb_path_is_svg(const char *path);

/* Document outline (markdown headings, populated each render). Count is 0 when
 * not viewing markdown or the doc has no headings. `fb_md_outline_get` copies
 * the heading label into `out` (nul-terminated) and returns its level (1..6),
 * or 0 if `idx` is out of range. `fb_md_scroll_to_heading` scrolls the viewer
 * so heading `idx` sits at the top (clamped to the content bounds). */
u32  fb_md_outline_count(const FileBrowser *fb);
u8   fb_md_outline_get(const FileBrowser *fb, u32 idx, char *out, usize out_cap);
void fb_md_scroll_to_heading(FileBrowser *fb, u32 idx);

/* If a code-block Copy button is under (vx,vy), return its code text (pointer
 * into the parse arena — NOT nul-terminated) plus length and true; else false.
 * Caller copies/terminates before handing it to the clipboard. */
/* Remaining strength of the Copy-confirmation flash, 1 → 0 (0 = none). Solid
 * for most of its life then fades, so the confirmation is readable rather than
 * a blink. */
f32 fb_md_copy_flash(const FileBrowser *fb, f64 now);

bool fb_md_hit_copy(const FileBrowser *fb, f32 vx, f32 vy,
                    const u8 **out_text, u32 *out_len);

/* Helpers */
const char *fb_format_size(u64 bytes);
const char *fb_file_icon(const char *name, bool is_dir);
FileViewMode fb_detect_mode(const char *filename);

/* The file-type icon, as ONE thing drawn in two places — the explorer row and
 * the editor tab's pill — so a file cannot wear one mark in the list and
 * another in the strip. A FILE wears its Seti UI icon in Seti's colour
 * (ui/seti_icons.h). A file Seti has no rule for keeps the logo it had when
 * there is one (the devicon PNGs compiled into fb_asset_icons.c: .astro, .nix,
 * …) and takes Seti's default mark otherwise. A folder, and every kind a caller
 * names itself, is the monoline vector icon of that kind.
 *
 * FbFileIcon is what the three functions pass around: an FbAssetIconKind as it
 * is, or — with FB_FILE_ICON_SETI set — a Seti icon in the low byte and its
 * palette colour in the next. 0 is FB_ASSET_ICON_NONE either way.
 *
 *   fb_file_icon_kind      — resolve a basename (or a directory) to its icon.
 *   fb_file_icon_ink_left  — how far left of an ink rail the icon's box
 *                            starts at `size`, for callers anchoring by INK
 *                            rather than by box (FB_ROW_PAD_PT in layout.h
 *                            says why).
 *   fb_file_icon_draw      — paint it in the size×size box at (x, y). `tint`
 *                            colours a vector icon and a silhouette logo
 *                            outright; a Seti icon and a full-colour logo
 *                            keep their own colour and take only its alpha,
 *                            so hidden entries still dim. */
typedef u32 FbFileIcon;
#define FB_FILE_ICON_SETI 0x10000u
FbFileIcon fb_file_icon_kind(const char *name, bool is_dir);
f32  fb_file_icon_ink_left(FbFileIcon icon, f32 size);
void fb_file_icon_draw(Renderer *r, FbFileIcon icon, f32 x, f32 y,
                       f32 size, Color tint);

/* Editor status-bar labels. Both return static/interned strings — copy before
 * the next call if you need to hold on to them. */
const char *fb_language_label(const char *path);          /* "C", "Python", … */
const char *fb_eol_label(const char *content, usize len); /* "LF" / "CRLF" */
void fb_entry_path(const FileBrowser *fb, const FileEntry *entry, char *out, usize out_size);

/* Editor operations */
void fb_editor_insert_char(FileBrowser *fb, u32 codepoint);
void fb_editor_backspace(FileBrowser *fb);
void fb_editor_delete(FileBrowser *fb);
void fb_editor_newline(FileBrowser *fb);
/* Replace `oldlen` bytes at byte-offset `off` with `newbytes[newlen]`. Shared
 * splice primitive (does not touch undo/cursor). */
bool fb_editor_splice(FileBrowser *fb, usize off, usize oldlen,
                      const char *newbytes, usize newlen);
/* Insert UTF-8 text at the cursor as one coalesced edit. Its line breaks —
 * LF, CRLF, a lone CR — are rewritten as the file's own (view_crlf). */
void fb_editor_insert_text(FileBrowser *fb, const char *utf8, usize len);
/* Paste the system clipboard at the cursor as a single undo unit. */
void fb_editor_paste(FileBrowser *fb);

/* Markdown formatting shortcuts (edit mode). */
void fb_editor_wrap_or_insert(FileBrowser *fb, const char *open, const char *close);
void fb_editor_make_link(FileBrowser *fb);
void fb_editor_toggle_line_prefix(FileBrowser *fb, const char *prefix);

/* ---- In-document Find / Replace (src/ui/fb_find.c) ---- */
void fb_find_open(FileBrowser *fb, bool replace_mode);
void fb_find_close(FileBrowser *fb);
void fb_find_recompute(FileBrowser *fb);     /* rescan view_content for the query */
/* Rescan when the text changed since the match list was built (the list is
 * byte offsets; an edit moves them). Next, Prev, both Replaces and the bar's
 * own draw do this themselves — call it after an edit made while the bar is
 * open so the frame that follows highlights the right bytes. True when it
 * rescanned; nothing happens while the bar is closed. */
bool fb_find_sync(FileBrowser *fb);
void fb_find_next(FileBrowser *fb);
void fb_find_prev(FileBrowser *fb);
/* Replace the current match and move to the next one PAST what was written
 * (none left: no current match, and a further call returns false). */
bool fb_find_replace_one(FileBrowser *fb);
/* Replace every match in the text — all of them, however many: it scans the
 * text itself and is not bound by the match list's cap. One undo unit. */
void fb_find_replace_all(FileBrowser *fb);
/* Append a UTF-8 char to the focused find/replace field; recomputes matches. */
void fb_find_input_char(FileBrowser *fb, u32 cp);
void fb_find_input_backspace(FileBrowser *fb);
/* Draw the find bar overlay at the top of the viewer content area. */
void fb_find_render(FileBrowser *fb, Renderer *r, f32 x, f32 y, f32 w,
                    f32 dpi, const Theme *theme, f32 opacity);
/* Byte offset of the cursor within view_content. */
usize fb_editor_cursor_offset(const FileBrowser *fb);
/* Draw the autocomplete popup anchored under the caret. */
void fb_ac_render(FileBrowser *fb, Renderer *r, f32 dpi, const Theme *theme, f32 opacity);
/* Read-mode highlight: shade match glyphs using md_glyph_rects (src_off). */
void fb_find_draw_matches(FileBrowser *fb, Renderer *r, const Theme *theme);
/* Edit-mode highlight for one raw source line [line_off, line_off+line_len). */
void fb_find_draw_line(const FileBrowser *fb, Renderer *r, usize line_off,
                       i32 line_len, f32 lx, f32 cy, f32 lcw, f32 lch,
                       const Theme *theme);
/* Markdown-aware Enter: auto-continues list/checkbox markers (falls back to a
 * plain newline outside lists). */
void fb_md_editor_newline(FileBrowser *fb);
/* Move the live-preview caret to the clicked (framebuffer) point. */
bool fb_md_live_locate(FileBrowser *fb, f32 mx, f32 my);
/* Move the code/plain-text editor caret to the clicked (framebuffer) point.
 * Uses the geometry cached by the last fb_render_viewer; UTF-8 aware
 * (cursor_col is a byte offset, the click x is a visual column). Returns
 * false for markdown/image/graph views or before the first render. */
bool fb_editor_click_caret(FileBrowser *fb, f32 mx, f32 my);
void fb_editor_move(FileBrowser *fb, i32 dcol, i32 drow);
/* Returns true when the document is written. Returns false when there is
 * nothing to save or when saving is refused or fails; `FileBrowser.notice`,
 * `save_result`, and `save_errno` describe the reason. A repeated save within
 * the guard interval allows overwriting a changed or read-only file. */
bool fb_editor_save(FileBrowser *fb);
void fb_editor_toggle(FileBrowser *fb);
void fb_editor_undo(FileBrowser *fb);
void fb_editor_redo(FileBrowser *fb);
void fb_editor_push_undo(FileBrowser *fb);

/* Bytes the undo system currently retains: every frame's spliced bytes plus
 * the pending span's committed bytes. The document itself is not counted — it
 * is bounded separately. */
usize fb_undo_retained_bytes(const FileBrowser *fb);
void fb_editor_reset_undo(FileBrowser *fb);

/* ---- The editor's selection ---------------------------------------------
 * Byte offsets into view_content; the range is [from, to) with from < to. */
bool  fb_editor_sel_active(const FileBrowser *fb);
void  fb_editor_sel_clear(FileBrowser *fb);
bool  fb_editor_sel_range(FileBrowser *fb, usize *from, usize *to);
/* The selected text, malloc'd and NUL-terminated (the file's own line breaks),
 * or NULL when nothing is selected. */
char *fb_editor_sel_text(FileBrowser *fb, usize *len);
void  fb_editor_select_all(FileBrowser *fb);
void  fb_editor_select_word(FileBrowser *fb);   /* the word, blank run or mark under the caret */
void  fb_editor_select_line(FileBrowser *fb);   /* the caret's line, with its break */
/* Copies or cuts the selection to the clipboard. A cut deletes the text only
 * after the copy succeeds. Returns true when a selection was handled;
 * otherwise the caller falls back to the line. */
bool  fb_editor_copy_selection(FileBrowser *fb);
bool  fb_editor_cut_selection(FileBrowser *fb);

/* A caret motion, with or without extending the selection (Shift). Without,
 * Left / Right over a selection collapse it to its edge and go nowhere else. */
typedef enum {
    FB_MOVE_LEFT, FB_MOVE_RIGHT, FB_MOVE_UP, FB_MOVE_DOWN,
    FB_MOVE_WORD_LEFT, FB_MOVE_WORD_RIGHT,
    FB_MOVE_LINE_START, FB_MOVE_LINE_END,
    FB_MOVE_DOC_START, FB_MOVE_DOC_END,
    FB_MOVE_PAGE_UP, FB_MOVE_PAGE_DOWN,
} FbMotion;
void  fb_editor_motion(FileBrowser *fb, FbMotion m, bool extend);

/* The pointer. `clicks` 1 / 2 / 3 = caret, word, line; `extend` (Shift) grows
 * the selection from the caret to the point. fb_editor_drag follows the pointer
 * with the button held and returns -1 / 0 / 1 when it is above / inside /
 * below the text, in which case the caller calls fb_editor_drag_scroll on a
 * clock for as long as the button is held. */
bool  fb_editor_press(FileBrowser *fb, f32 mx, f32 my, i32 clicks, bool extend);
i32   fb_editor_drag(FileBrowser *fb, f32 mx, f32 my);
void  fb_editor_drag_scroll(FileBrowser *fb, i32 dir, f32 mx);

/* Editing over a selection / a line. */
void  fb_editor_indent(FileBrowser *fb, bool outdent);     /* Tab / Shift+Tab, Cmd+] / Cmd+[ */
void  fb_editor_tab(FileBrowser *fb, bool back);            /* the Tab key: lines, a list item, or four spaces */
void  fb_editor_move_lines(FileBrowser *fb, i32 dir);      /* Option+Up / Down: -1 / +1 */
void  fb_editor_open_line(FileBrowser *fb, bool above);    /* Cmd+Enter / Cmd+Shift+Enter */
void  fb_editor_toggle_comment(FileBrowser *fb);           /* Cmd+/ */
void  fb_editor_kill_to_line_end(FileBrowser *fb);         /* Ctrl+K */
bool  fb_editor_md_list_line(FileBrowser *fb);             /* the caret's line is a list item */
char *fb_editor_get_line(FileBrowser *fb, i32 line);
void fb_editor_duplicate_line(FileBrowser *fb);
void fb_editor_copy_line(FileBrowser *fb);
void fb_editor_cut_line(FileBrowser *fb);
void fb_editor_goto_line_start(FileBrowser *fb);
void fb_editor_goto_line_end(FileBrowser *fb);
/* macOS-native word / line / document editing (Option+arrows, Option/Cmd+Delete,
 * Cmd+arrows). cursor_col is a byte offset; words break on whitespace. */
void fb_editor_move_word_left(FileBrowser *fb);
void fb_editor_move_word_right(FileBrowser *fb);
void fb_editor_delete_word_left(FileBrowser *fb);
void fb_editor_delete_word_right(FileBrowser *fb);
void fb_editor_delete_to_line_start(FileBrowser *fb);
void fb_editor_goto_doc_start(FileBrowser *fb);
void fb_editor_goto_doc_end(FileBrowser *fb);

/* File operations (in fileops.c) */
bool fb_download_file(FileBrowser *fb, i32 index, const char *local_dir);
bool fb_upload_file(FileBrowser *fb, const char *local_path);
bool fb_delete_file(FileBrowser *fb, i32 index);
bool fb_mkdir(FileBrowser *fb, const char *name);
bool fb_create_file(FileBrowser *fb, const char *name);
bool fb_rename(FileBrowser *fb, i32 index, const char *new_name);
/* The local half of fb_rename: never replaces another file (errno EEXIST),
 * lets a case-only change of the same file through. */
bool fb_rename_local_noreplace(const char *from, const char *to);
/* Move a file, a folder or a link to another folder — on the same volume a
 * rename, on another one a copy followed by removing the source (which
 * fb_rename_local_noreplace cannot do: it answers EXDEV). Never replaces
 * (errno EEXIST). True only when ALL of it moved: a folder whose copy left
 * something behind, or failed, keeps its source whole. */
bool fb_move_local_noreplace(const char *from, const char *to);
bool fb_copy_local(const char *src, const char *dst);

/* File clipboard entry — one source file/dir registered via Cmd+C/X.
 * Source may live in a different FileBrowser instance than the paste target
 * (e.g. cut from SFTP tab A, paste into SFTP tab B, or into a local tab).
 * `session` is opaque; fb_paste reopens the SFTP scope as needed. */
typedef struct FileClipboardItem {
    FileBrowserSource  source;
    char               path[FB_MAX_PATH * 2];  /* absolute source path */
    char               name[256];              /* basename, used as default dst name */
    bool               is_dir;
    void              *session;                /* retained Session* when source == SFTP */
} FileClipboardItem;

#define FB_CLIP_MAX 64

/* Full clipboard — holds up to FB_CLIP_MAX items plus a global is_cut flag
 * shared across the batch. `has` is `count > 0`; kept as a cached bool so hot
 * UI paths (render ctx-menu dim state) skip the count load.
 *
 * `items` is HEAP-allocated on the first Cmd+C/X and sized to the actual
 * selection (see app_fb_copy in main.c), not to FB_CLIP_MAX. Inline it cost
 * FB_CLIP_MAX * sizeof(FileClipboardItem) ≈ 145 KB inside every AppState —
 * ~29 % of the struct — resident from launch even for users who never copy a
 * file. Same lazy-alloc pattern as the vault's key_list / passphrase_cache.
 * Invariants: `items == NULL` ⇒ `count == 0`, and `count` never exceeds the
 * allocated element count (which is itself clamped to FB_CLIP_MAX). */
typedef struct FileClipboard {
    bool               has;
    bool               is_cut;
    i32                count;
    FileClipboardItem *items;
} FileClipboard;

/* Paste every item in `clip` into `dst_fb->cwd`.
 * Handles all 4 source×dest combos (local/sftp × local/sftp) and recurses
 * into directories on cross-source transfers. After a successful cut-paste
 * every source item is removed. Returns true when all items succeed. */
bool fb_paste(FileBrowser *dst_fb, const FileClipboard *clip);

/* Paste a single item — used internally by fb_paste and by the drag-drop
 * handler (which builds an item on the fly from an entry). True only when the
 * whole item landed; fb_paste_item_report says which kind of "no" it was. */
bool fb_paste_item(FileBrowser *dst_fb, const FileClipboardItem *item, bool is_cut);

/* What became of one item. `links` counts the entries its walk left behind on
 * purpose; `err` is the errno of a failure where one is known (EEXIST: the
 * destination already has that name), else 0; `dst` is where the item was to
 * land — after a successful MOVE, the path an open document should follow
 * (fb_view_path_moved). */
typedef struct FbPasteReport {
    FbPasteOutcome outcome;
    i32            links;
    int            err;
    char           dst[FB_MAX_PATH * 2];
} FbPasteReport;

/* fb_paste_item, saying what happened (ui/paste_report.h): OK, copied except
 * links it would not follow, or failed. `report` may be NULL. Feed the result
 * to fb_paste_tally_add() and the status line can tell a paste that lost an
 * item from one that only skipped links. */
FbPasteOutcome fb_paste_item_report(FileBrowser *dst_fb, const FileClipboardItem *item,
                                    bool is_cut, FbPasteReport *report);

/* Force local moves to receive `EXDEV` from `rename`, simulating a
 * cross-volume move. The override is process-wide; disable it after use.
 */
void fb_fileops_test_force_exdev(bool on);
/* How many entries the remote walks of the pastes since the last call left
 * behind ON PURPOSE — a link to a folder, a dangling link, a device (see the
 * rules in fileops.c). Such a paste reports failure, so a move keeps its
 * source; this is what lets the UI say why. Resets the count. */
i32  fb_paste_take_skipped(void);

#endif
