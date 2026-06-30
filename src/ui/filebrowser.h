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
#include <pthread.h>     /* async graph-build worker handle */
#include <stdatomic.h>

/* Forward declarations to avoid pulling the markdown headers into every
 * translation unit that includes filebrowser.h. */
typedef struct MdDoc       MdDoc;
typedef struct MdImageCache MdImageCache;
typedef struct MdLinkRect   MdLinkRect;
typedef struct MdTaskRect   MdTaskRect;
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
    /* Graph and markdown views keep the file list a narrow sidebar so the
     * renderer fills the window (unless the user has dragged the divider). */
    if (narrow_list && viewer_ratio <= 0.0f) {
        f32 lw = VIEWER_LIST_MIN_PT * dpi;
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

/* Upper bound on glyph rects collected per render for text selection. Only
 * glyphs in visible blocks are emitted, so a screenful (plus edge bleed) is
 * the worst case; surplus glyphs are dropped (selection past the cap simply
 * isn't recorded). */
#define FB_MD_GLYPH_CAP 8192

/* Upper bound on document-outline headings recorded per render. */
#define FB_MD_OUTLINE_CAP 512

#define FB_MAX_ENTRIES 1024
#define FB_MAX_PATH    1024

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
    u64     size;
    u32     permissions;
    u64     mtime;
    bool    is_dir;
    bool    is_symlink;
    i32     depth;         /* indentation level for tree view */
    bool    expanded;      /* if dir, is it expanded? */
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

/* Syntax token types for code highlighting */
typedef enum {
    SYN_NORMAL,
    SYN_KEYWORD,
    SYN_STRING,
    SYN_COMMENT,
    SYN_NUMBER,
    SYN_FUNCTION,
    SYN_TYPE,
    SYN_OPERATOR,
    SYN_PREPROCESSOR,
} SyntaxType;

typedef struct {
    i32        start;   /* byte offset in line */
    i32        len;
    SyntaxType type;
} SyntaxSpan;

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

typedef struct {
    bool            open;
    FileBrowserSource source;
    char            cwd[FB_MAX_PATH];
    FileEntry      *entries;    /* heap-allocated on navigate, freed on destroy */
    i32             entry_count;
    i32             entry_cap;
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
    f32             width;          /* sidebar width in pixels */

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
    SyntaxSpan     *view_spans;      /* per-line syntax spans */
    i32             view_line_count;

    /* Editor state */
    bool            editor_mode;      /* true = editable, false = read-only viewer */
    i32             cursor_line;      /* cursor line (0-based) */
    i32             cursor_col;       /* cursor column (0-based) */
    bool            modified;         /* unsaved changes */
    i32             edit_counter;     /* per-instance undo-snapshot throttle */

    /* Delta-based undo stack.
     *
     * Each frame stores the splice that transforms state[i] → state[i+1]
     * (old bytes at offset replaced by new bytes), not a full buffer copy.
     * Storage per frame is typically a few bytes — a 2 MiB file with 32
     * snapshots used to cost ~66 MiB; it now costs ~2 MiB (one base mirror)
     * plus tiny deltas.
     *
     * - `undo_base` holds state[0] (original file, or rolled-forward base
     *   after the stack fills and the oldest frame is folded into it).
     * - `undo_tip` mirrors state[undo_count] (the top of the stack). We
     *   maintain it incrementally on push/truncate so `push_undo` can diff
     *   `undo_tip` against `view_content` in O(buffer) without replaying.
     * - `undo_frames[i]` is the forward diff from state[i] → state[i+1].
     * - `undo_pos` is the user-visible position: view_content == state[undo_pos]
     *   in steady state, or may be past the tip if the user has made draft
     *   edits that haven't triggered a push_undo yet. */
    struct FbUndoFrame {
        usize offset;       /* splice position in state[i] */
        usize old_len;      /* bytes removed at offset (may be 0) */
        usize new_len;      /* bytes inserted at offset (may be 0) */
        char *old_bytes;    /* owned; NULL iff old_len == 0 */
        char *new_bytes;    /* owned; NULL iff new_len == 0 */
        i32   cursor_line;  /* cursor AT state[i+1] */
        i32   cursor_col;
    } undo_frames[32];
    char           *undo_base;            /* full copy of state[0] */
    usize           undo_base_size;
    i32             undo_base_cursor_line;
    i32             undo_base_cursor_col;
    char           *undo_tip;             /* full copy of state[undo_count] */
    usize           undo_tip_size;
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
    bool          md_raw_mode;     /* false = rendered, true = raw source */

    /* Click-targets recorded by md_render each frame. Rects sit in viewport
     * coordinates (framebuffer pixels). Cleared and refilled per frame; safe
     * to consult immediately after fb_render_viewer returns. URL pointers
     * borrow from the parse arena, valid until the doc is destroyed. */
    MdLinkRect   *md_link_rects;
    u32           md_link_count;
    /* Clickable task-checkbox rects, same per-frame lifecycle as md_link_rects. */
    MdTaskRect   *md_task_rects;
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
    f32           live_cw, live_ch, live_dpi; /* live-preview metrics, last render (click→cursor) */
    /* Scroll value the glyph rects were captured at. Used by
     * md_sel_index_at to convert rects back to doc space without
     * accidentally applying the current scroll twice. Without this,
     * a scroll-mid-drag offsets every endpoint mapping by the scroll
     * delta and the selection grabs the wrong glyphs for one frame. */
    f32           md_glyph_scroll_px;

    /* Document outline (headings) collected each markdown render, in document
     * order. Drives the command-palette "Go to Heading" quick-nav. */
    MdOutlineItem *md_outline;
    u32            md_outline_count;

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
    /* User-controlled zoom (1.0 = fit to viewer area). Reset to 1.0 on
     * every open so a small image doesn't carry a previous file's zoom. */
    f32           view_image_zoom;
    f32           view_image_pan_x;
    f32           view_image_pan_y;

    /* Graph view (FVIEW_GRAPH): force-directed knowledge graph of the
     * folder's .md files, CPU-rasterized + drawn via the image path. */
    MdGraph      *graph;
    /* Async build: md_graph_build does recursive disk I/O + parsing, which on a
     * slow / network volume (plus the macOS file-access prompt) would freeze the
     * main thread. So the scan runs on a worker; the view shows a spinner until
     * graph_pending is ready, then the main thread swaps it into `graph`. */
    pthread_t     graph_build_thread;
    bool          graph_building;            /* main-thread flag: build in flight */
    _Atomic bool  graph_build_done;          /* worker → main: finished */
    MdGraph      *graph_pending;             /* worker writes the result here */
    char          graph_build_dir[FB_MAX_PATH];
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
} FileBrowser;

/* Init / lifecycle */
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
bool fb_navigate(FileBrowser *fb, const char *path);
bool fb_navigate_sftp(FileBrowser *fb, void *sftp_handle, const char *path);
void fb_go_up(FileBrowser *fb);
void fb_refresh(FileBrowser *fb);

/* File operations */
bool fb_open_file(FileBrowser *fb, i32 index);
void fb_close_viewer(FileBrowser *fb);
/* Close variant: keep_graph=true preserves fb->graph for the paired
 * fb_reenter_graph (return-to-graph); every other caller uses fb_close_viewer. */
void fb_close_viewer_ex(FileBrowser *fb, bool keep_graph);

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
void        fb_graph_scroll(FileBrowser *fb, f32 dy, f32 rel_x, f32 rel_y, f32 w, f32 h);
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
/* Open an arbitrary .md by absolute path in this browser's markdown viewer
 * (navigates to its folder, exits graph mode). Returns true on success. */
bool        fb_open_md_path(FileBrowser *fb, const char *abs_path);

/* Rendering */
void fb_render_sidebar(FileBrowser *fb, Renderer *r, f32 x, f32 y, f32 w, f32 h,
                       const Color *bg, const Color *fg, const Color *sel,
                       const Color *dim, const Color *accent, f32 dpi,
                       f32 hover_x, f32 hover_y);
void fb_render_viewer(FileBrowser *fb, Renderer *r, f32 x, f32 y, f32 w, f32 h, f32 dpi, const Theme *theme, f32 opacity);

/* Input */
bool fb_handle_click(FileBrowser *fb, f32 rel_x, f32 rel_y, f32 w, f32 h, f32 dpi);
void fb_handle_scroll(FileBrowser *fb, f32 dy, bool precise);

/* Markdown viewer link hit-test. (vx, vy) are framebuffer pixel coords. If a
 * link was rendered under the point, copies its URL into `out` (nul-terminated,
 * truncated to out_cap-1), and returns true. Rects must be populated by a
 * prior fb_render_viewer for the same frame (or the most recent one when
 * polling on mouse-move). Out remains untouched on miss. */
bool fb_md_hit_link(const FileBrowser *fb, f32 vx, f32 vy,
                    char *out, usize out_cap, u8 *out_kind);

/* Click a rendered task-list checkbox to toggle "[ ]"<->"[x]" in the source file
 * (read-mode only). Reparses + persists on a hit. Returns true if a box was hit. */
bool fb_md_toggle_task_at(FileBrowser *fb, f32 vx, f32 vy);

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

/* Read/Edit (preview/write) toggle for a markdown viewer.
 * Entering Edit makes the raw buffer editable; leaving it re-parses so the
 * rendered preview reflects the edits. fb_md_edit_active reports write mode. */
void fb_md_toggle_edit(FileBrowser *fb);
bool fb_md_edit_active(const FileBrowser *fb);

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
bool fb_md_hit_copy(const FileBrowser *fb, f32 vx, f32 vy,
                    const u8 **out_text, u32 *out_len);

/* Helpers */
const char *fb_format_size(u64 bytes);
const char *fb_file_icon(const char *name, bool is_dir);
FileViewMode fb_detect_mode(const char *filename);
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
/* Insert UTF-8 text at the cursor as one coalesced edit (CRLF→LF normalized). */
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
void fb_find_next(FileBrowser *fb);
void fb_find_prev(FileBrowser *fb);
bool fb_find_replace_one(FileBrowser *fb);
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
void fb_editor_move(FileBrowser *fb, i32 dcol, i32 drow);
bool fb_editor_save(FileBrowser *fb);
void fb_editor_toggle(FileBrowser *fb);
void fb_editor_undo(FileBrowser *fb);
void fb_editor_redo(FileBrowser *fb);
void fb_editor_push_undo(FileBrowser *fb);
void fb_editor_reset_undo(FileBrowser *fb);
void fb_editor_select_all(FileBrowser *fb);
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
    void              *session;                /* owning Session* when source == SFTP */
} FileClipboardItem;

#define FB_CLIP_MAX 64

/* Full clipboard — holds up to FB_CLIP_MAX items plus a global is_cut flag
 * shared across the batch. `has` is `count > 0`; kept as a cached bool so hot
 * UI paths (render ctx-menu dim state) skip the count load. */
typedef struct FileClipboard {
    bool               has;
    bool               is_cut;
    i32                count;
    FileClipboardItem  items[FB_CLIP_MAX];
} FileClipboard;

/* Paste every item in `clip` into `dst_fb->cwd`.
 * Handles all 4 source×dest combos (local/sftp × local/sftp) and recurses
 * into directories on cross-source transfers. After a successful cut-paste
 * every source item is removed. Returns true when all items succeed. */
bool fb_paste(FileBrowser *dst_fb, const FileClipboard *clip);

/* Paste a single item — used internally by fb_paste and by the drag-drop
 * handler (which builds an item on the fly from an entry). */
bool fb_paste_item(FileBrowser *dst_fb, const FileClipboardItem *item, bool is_cut);

#endif
