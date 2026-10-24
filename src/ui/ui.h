/*
 * Liu - UI system
 * Tab management, split panes, sidebar for host/SFTP, themed rendering.
 */
#ifndef UI_H
#define UI_H

#include "core/types.h"
#include "ui/paste_confirm.h"
#include <time.h>
#include "core/poll_budget.h"
#include "core/poll_gate.h"
#include "ui/sleep_policy.h"
#include "ui/wake_input.h"
#include "ui/agent_busy.h"
#include "notify/agent_signal.h"
#include "ui/git_poll.h"
#include "ui/drag_spring.h"
#include "ui/sidebar_layout.h"
#include "ui/palette_layout.h"
#include "core/config.h"
#include "core/keybind.h"
#include "terminal/terminal.h"
#include "terminal/url.h"
#include "ui/url_hover.h"
#include "terminal/prompt_capture.h"
#include "ssh/ssh_session.h"
#include "ssh/keygen.h"
#include "core/sites.h"
#include "vault/vault.h"
#include "renderer/renderer.h"
#include "ui/render_cpu_bytes.h"
#include "ui/settings.h"
#include "ui/filebrowser.h"
#include "ui/fb_menu.h"
#include "ui/ctx_menu.h"
#include "ui/icon.h"
#include "browser/browser_types.h"
#include "ui/anim.h"
#include "ui/lightbox.h"
#include "ui/sketch.h"
#include "ui/dpi_refresh.h"
/* Forward only: the HUD's counters are a heap block hung off AppState, and
 * ui.h is included by nearly every file in the tree. */
struct BenchMetrics;
#include "ui/textedit.h"
#include "update/updater.h"
#include "notify/notify_server.h"
#include "translate/translate_segment.h"
#include "translate/translate_draft.h"
#include "ui/translate_fx.h"
#include "ui/pane_drop_anim.h"
#include "ui/ime_target.h"
#include "ui/toast_stack.h"
#include "translate/translate_history_cleanup.h"
#include "ui/pane_sync.h"

/* Forward decl — full struct lives in core/agent_detect.h. AppState carries
 * a heap-allocated array of these for the Cmd+K → Create Theme picker. */
struct AgentInfo;

#define MAX_TABS 32
#define MAX_TAB_GROUPS 8
#define MAX_SPLIT_PANES 8
/* Cap clickable image badges per frame across all visible panes. Increment
 * `AppState.img_badge_dropped` for each badge omitted after the cap. */
#define MAX_IMG_BADGES 128
#define MAX_SPLIT_NODES (MAX_SPLIT_PANES * 2 - 1)

/* =========================================================================
 * Per-window modal animation state
 *
 * Each window owns its open/close triplet. With N windows all rendered in
 * one frame, a shared triplet makes modal_anim_progress() see
 * `want_open != *was_open` on EVERY call: the window that did not open the
 * dialog takes the falling edge and paints a fully opaque, inert panel it
 * never opened, the window that DID open it takes the rising edge and paints
 * nothing, and the per-frame re-arm keeps anim_global_active() true so the
 * loop never idles.
 *
 * SiteManagerUI has the same shape (open_anim/close_anim/was_open on the
 * struct); here it is collapsed into one array so a new dialog costs one
 * enum entry.
 * ========================================================================= */
typedef enum {
    MODAL_PALETTE = 0,
    MODAL_BROADCAST,
    MODAL_SEARCH,
    MODAL_PORT_FORWARD,
    MODAL_SSH,
    MODAL_CREATE_THEME,
    MODAL_KBI,
    MODAL_KEY_MANAGER,
    MODAL_HOST_KEY,
    MODAL_CLOSE_CONFIRM,
    MODAL_PASTE_CONFIRM,       /* "paste N lines?" — see ui/paste_confirm.h */
    MODAL_KNOWN_HOSTS,
    MODAL_PASSPHRASE,
    MODAL_TRANSCRIPT,
    MODAL_AGENT_PICKER,
    MODAL_SIDEBAR_CUSTOMIZE,   /* the "Customize sidebar" popover */
    MODAL_COUNT
} ModalId;

typedef struct {
    bool was_open;    /* edge tracker owned by modal_anim_progress() */
    Anim open;
    Anim close;
} ModalAnim;

/* =========================================================================
 * Tab Group -- named groups for organizing tabs
 * ========================================================================= */

typedef struct {
    char  name[64];
    Color color;          /* group indicator color */
    bool  collapsed;      /* if true, tabs in this group are hidden in the tab bar */
    bool  used;           /* whether this group slot is in use */
    Anim  collapse_anim;  /* expand/collapse transition (members shrink/grow) */
} TabGroup;

/* Monotonic nanoseconds, for the per-frame render counters below.
 *
 * Unconditional rather than gated on a probe flag: four clock reads per
 * pane per frame cost tens of nanoseconds even for an eight-pane window,
 * and a counter that only exists when a probe is enabled is rarely read. */
static inline u64 liu_render_probe_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}

/* =========================================================================
 * Per-terminal render cache
 *
 * Per-row packed caches for bg rects and glyph instances. Each row grows only
 * to the amount of content it actually needs, instead of reserving rows*cols
 * slots up front for every pane.
 * ========================================================================= */

typedef struct {
    RectInstance **bg_rows;
    i32           *bg_counts;
    i32           *bg_caps;
    GlyphInstance **glyph_rows;
    RendererRetainedBatch *gpu_glyphs, *gpu_backgrounds, *gpu_decorations;
    u64 content_revision, decoration_revision;
    u64 bg_revision, glyph_revision;
    u64 *bg_versions, *glyph_versions; /* one allocation, owned by bg_versions */
    RectInstance *spare_backgrounds;
    GlyphInstance *spare_glyphs;
    i32 spare_bg_cap, spare_glyph_cap;
    bool row_allocation_failed;
    const void *atlas_identity;
    void *prepare_job;          /* owned immutable snapshot; no AppState borrow */
    u32 prepare_discards;
    u64 prepare_retry_after_ns;
    i32           *glyph_counts;
    i32           *glyph_caps;
    /* Underline / strikethrough / overline, cached on the SAME dirty-row
     * signal as the two above. It belongs here because its inputs are exactly
     * theirs — the visible row's cells, the sparse underline map, and the BiDi
     * reordering — so anything that invalidates a glyph row invalidates its
     * decorations too. Uncached, this pass walked all rows x cols on EVERY
     * painted frame, including the frames a settings animation or a blinking
     * caret drives while the terminal itself has not changed. */
    RectInstance **deco_rows;
    i32           *deco_counts;
    i32           *deco_caps;
    /* "Does this row name a local image file, and where does its badge go?" —
     * cached on the SAME dirty-row signal again, and for the same reason: the
     * answer changes only when the row's cells do, but the scan behind it walks
     * the row's tokens and would otherwise run rows x cols on every painted
     * frame. It is the more important of the two to cache, because an agent
     * streaming into the alt screen dirties its live region continuously and
     * the scan ends in a stat() (memoised in img_link.c, but still). */
    u8            *img_rows;        /* 1 = this row names an image file */
    i32           *img_badge_cols;  /* column the badge is drawn at */
    /* 1 = a glyph in this row drew as the SPACE placeholder while the raster
     * worker finishes it, so a completed rasterisation REPAIRS this row and
     * only rows like it.
     *
     * A screen scrolling CJK or mixed prose receives new glyphs on nearly
     * every row anyway. The case this serves is a STATIC screen whose single
     * prompt row gains new codepoints (typing non-Latin text), where marking
     * every row would rebuild almost only rows with nothing new. */
    u8            *pending_rows;
    i32            cached_rows;
    i32            cached_cols;
    f32            cached_cw, cached_ch;     /* invalidation: font size change */
    f32            cached_origin_x, cached_origin_y; /* fractional pixel phase, not pane translation */
    bool           all_rows_dirty;           /* forces full rebuild next frame */
    /* The pane was flooding when its rows were last built, so they were built
     * without ligatures (term_pane_draw); the frame it stops, all of them are
     * rebuilt with them. */
    bool           lig_flood;
    /* AppState.frame_seq of the frame whose pane draw last put these rows on
     * the GPU. A second appearance of the terminal in that same frame (a drop
     * preview) replays the retained batches instead of building anything — and
     * may do so ONLY then: a pane draw that returned before its emit (deferred
     * prepare, atlas moved under the build) leaves rows the GPU must not see. */
    u64            emit_seq;

    /* THIS TERMINAL'S palette, resolved once.
     *
     * OSC 4 changes this terminal's palette only, never the process-global
     * g_ansi_colors, so a palette set by one shell does not repaint the indexed
     * colours of any other terminal and two panes can hold different palettes.
     *
     * Built from term->palette when it changes and read per cell, so a terminal
     * that never receives OSC 4 pays nothing: palette_lut_valid stays false and
     * the built-in global LUT answers. */
    Color         *palette_lut;      /* allocated only for a custom palette */
    const u32     *palette_source;   /* main-thread OOM fallback, never borrowed by a worker */
    bool           palette_lut_valid;
} TermRenderCache;

static inline usize term_render_cache_bytes(const TermRenderCache *c) {
    if (!c) return 0;
    return term_render_cache_bytes_from(c->cached_rows,c->bg_caps,c->glyph_caps,c->deco_caps,
        c->bg_rows,c->glyph_rows,c->deco_rows)+
        term_render_cache_aux_bytes(c->cached_rows,c->bg_versions!=NULL,
            c->spare_backgrounds?c->spare_bg_cap:0,c->spare_glyphs?c->spare_glyph_cap:0,c->palette_lut!=NULL);
}

/* ---- Diff dock (ui/git_diff.c) -------------------------------------------
 * State for the diff dock the status bar's git segment opens. There is no
 * file picker and no menu: the dock shows the whole worktree diff, top to
 * bottom, and nothing else.
 *
 * The dock parses `git diff` itself and paints it, rather than feeding
 * `--color=always` to a headless Terminal. A terminal grid WRAPS, and a diff
 * in a 460pt sidebar is mostly lines longer than the grid is wide, so every
 * other row would fold into a second, unindented row. Painting it directly
 * also buys the things a terminal cannot express: a line-number gutter,
 * per-row tinting that spans the full width, sticky file headers, and
 * horizontal scrolling instead of folding.
 *
 * GdLine / GdFile are private to git_diff.c — this header only needs to own
 * the pointers. */
struct GdLine;
struct GdFile;

typedef struct {
    bool  open;
    bool  loading;                 /* diff job in flight */

    char  root[1024];              /* worktree root the view is bound to */

    /* Parsed diff. `text` is the raw output every slice points into; all three
     * allocations are owned by the view. */
    char          *text;
    usize          text_len;
    /* Capacities, for the ledger (MEM_GIT): what malloc was asked for, which
     * is what the process holds — a row array that doubled to 2048 slots for
     * 1030 rows is holding 2048. Cleared with the arrays by gd_clear. */
    usize          text_cap;
    /* Worker-built row paint data: syntax spans and sparse
     * display-column checkpoints. Owned with the parsed model. */
    void          *paint_cache;
    usize          paint_cache_cap;
    bool           failed;
    bool           truncated;
    struct GdLine *lines;
    i32            line_count;
    i32            line_cap;
    struct GdFile *files;
    i32            file_count;
    i32            file_cap;
    i32            total_added, total_removed;
    i32            gutter_digits;  /* widest line number in the whole diff */
    i32            max_cols;       /* longest row, for the h-scroll bound */

    f32   scroll_px;               /* vertical, in pixels */
    f32   hscroll_px;              /* horizontal, in pixels */

    /* Counts are an early refresh hint. The worker fingerprint also detects
     * content revisions whose addition/deletion counts are unchanged. */
    i32   seen_added, seen_removed;

    /* Dock geometry. The view is a right-hand sidebar, not an overlay: the
     * terminal grid is narrowed by `width` while it is open (see
     * app_git_diff_dock_w), so it never covers output. 0 = use the default. */
    f32   width;
    bool  resizing;                /* divider drag in flight */

    /* Trackpad axis lock for this dock's scroller. Per view because the lock
     * survives ~200 ms of idle: shared, a vertical read in one window followed
     * by a sideways swipe in another inside that window would scroll the wrong
     * axis. */
    f64   scroll_axis_ts;
    i32   scroll_axis;             /* 0 undecided, 1 vertical, 2 horizontal */

    /* The diff worker and its one result slot (GdWorker, private to
     * git_diff.c), per view like the view itself: shared across windows, one
     * window's poll could install another dock's result or free its diff.
     * Allocated on first request, joined and freed by git_diff_free. */
    void *worker;
} GitDiffView;

/* ---- Tab popup (ui/tab_popup.c) ------------------------------------------
 * The panel that drops out from under the tab strip: the hover preview of a
 * tab's agents, and the agent-done notice.
 *
 * Per window, because everything in it is: `tab` indexes THAT window's tabs[],
 * the anchor/draw x are window-local pixels, and the dwell is driven from the
 * render path, which runs for every window every frame. As one process-wide
 * struct, the unfocused window's tab_popup_hover(app, -1, …) would reset the
 * dwell before the focused window's call could reach it, an agent finishing
 * in window B would pop a card in window A describing A's tab of the same
 * index, and two windows of different widths would fight over the glide
 * target. */
typedef struct {
    u8    kind;             /* PopupKind — 0 = none; private to tab_popup.c */
    i32   tab;              /* tab index the panel belongs to */

    i32   dwell_tab;        /* tab the pointer is resting on, -1 = none */
    f64   dwell_since;

    f32   anchor_cx;        /* centre of the tab's pill */
    f32   draw_cx;          /* eased x the panel is actually drawn at */
    bool  have_draw_cx;
    f64   glide_t;
    /* Side anchoring: with the tabs docked in the sidebar the panel opens to
     * the RIGHT of the hovered row rather than under the strip. anchor_x is
     * the sidebar's right edge, anchor_y the row's top; draw_y glides. */
    bool  anchor_side;
    f32   anchor_x, anchor_y;
    f32   draw_y;
    bool  have_draw_y;

    Anim  open_anim;
    Anim  close_anim;
    bool  want_open;

    /* DONE payload. */
    i32   done_tool;        /* ChatTool as i32 */
    i32   done_pane;
    char  done_title[96];   /* the chat's topic */
    f64   done_secs;        /* how long the agent worked */
    f64   done_until;       /* auto-dismiss deadline */
} TabPopup;

/* =========================================================================
 * Sites / dev-server manager — UI view state
 *
 * The backend runtime (SiteManager: process + headless log Terminal per site)
 * lives in AppState.site_mgr; this struct holds only the panel's view state.
 * One render cache is enough because only the selected site's log is shown;
 * it's reset whenever the displayed terminal changes.
 * ========================================================================= */

typedef struct {
    bool  active;            /* panel open (drives the modal animation) */
    i32   selected;          /* selected site index, -1 = none */
    i32   hover_row;         /* hovered list row, -1 = none */
    i32   hover_action;      /* hovered footer action, -1 = none */
    f32   list_scroll_px;    /* list scroll offset (pixels) */

    /* Inline command form shown in the log pane for a freshly added site. */
    bool  addform_active;
    i32   addform_site;      /* site index being edited */
    char  addform_buf[512];  /* editable command */
    i32   addform_len;

    /* Single log-pane render cache (only one site's log is visible). */
    TermRenderCache log_cache;
    Terminal       *log_cache_term;  /* terminal the cache was last built for */

    /* Modal entrance/exit animation. */
    Anim  open_anim;
    Anim  close_anim;
    bool  was_open;
} SiteManagerUI;

typedef struct {
    SessionType type;
    SSHConfig  *ssh_config;    /* heap copy for SSH/Mosh tabs, NULL for local */
    char       *cwd;           /* heap-allocated last known local cwd; NULL if unset */
    char       *snap_path;     /* heap-allocated scrollback snapshot path; NULL until written */
} TabSleepSnapshot;

/* =========================================================================
 * Sidebar mode
 * ========================================================================= */

typedef enum {
    SIDEBAR_HOSTS,
    SIDEBAR_SFTP,
    SIDEBAR_SNIPPETS,
} SidebarMode;

/* =========================================================================
 * Tab
 * ========================================================================= */

typedef enum {
    SPLIT_NONE = 0,
    SPLIT_H,   /* side by side */
    SPLIT_V,   /* top/bottom */
} SplitType;

typedef struct {
    bool      used;
    bool      leaf;
    SplitType split;
    f32       ratio;
    i32       pane;
    i32       first;
    i32       second;
} SplitLayoutNode;

typedef struct {
    f32 x, y, w, h;
} SplitAnimRect;

/* Laid-out rect of one split pane (framebuffer px). Also used by the tab
 * popup to mirror a tab's split geometry at thumbnail scale. */
typedef struct {
    f32 x, y, w, h;
} PaneRect;

/* Keep-alive hold for the "agent is working" gate, in seconds. Agent output
 * is bursty — token gaps, thinking pauses and tool calls all let the output
 * accumulator decay within ~0.5 s — so once a real burst crosses the threshold
 * the pane counts as working until this long past the last burst. Shared so
 * the tab popup can subtract it back out when it reports how long ago a pane
 * actually fell quiet. */
/* TAB_ACCENT_WORK_HOLD lives beside the two output levels it works
 * with, in ui/agent_busy.h: they are one policy. */

/* Per-pane foreground-agent state.
 *
 * "Which agent is in this tab and is it busy" is tracked per PANE, so a tab
 * running three agents in a split is not described by whichever pane was
 * clicked last, and the activity bar follows every pane. The tab-level
 * aggregates (title, accent bar, popup) are derived from this array each
 * poll. */
typedef struct {
    i32   tool;              /* ChatTool stored as i32; 0 = none */
    f64   tool_next_probe;   /* wall clock; throttles session_fg_process */
    char  chat_title[96];    /* live chat title from the tool's on-disk store */
    f64   title_next_probe;  /* wall clock; throttles the disk probe */
    u64   title_request;     /* one mailbox reference; moves with this pane */

    /* What the agent is doing right now, scraped off this pane's own screen by
     * terminal/agent_activity.c ("Reading lines 200-520 of tab_popup.c").
     *
     * It exists because `chat_title` is empty far more often than not: only
     * five of the CLIs Liu recognises publish a live title at all, and none of
     * them names a conversation before its first exchange finishes, so the tab
     * preview would otherwise say "untitled chat" for a pane that was just
     * prompted and is visibly working.
     *
     * Written ONLY while the pane is working, which is what keeps the model's
     * final answer out of it, and kept afterwards: once the turn ends, the last
     * thing the agent did is the best description of the tab there is. Cleared
     * with the rest of the pane's state when its agent changes or exits. */
    char  activity[96];
    f64   activity_next_probe;

    /* The most recent prompt submitted to this pane, shown by the Prompt
     * Recall badge.
     *
     * Capture it from Liu's input mirror because the agent screen may use an
     * alternate screen without scrollback, and agent CLIs do not share a
     * prompt-retrieval interface. Store it on Enter and clear it when the
     * pane's agent changes or exits. See terminal/prompt_capture.h. */
    PromptRecall prompt;
    /* Is the badge expanded into the full-text panel?
     *
     * Per pane rather than one "which panel is open" pointer in AppState,
     * because in PROMPT_RECALL_ALWAYS every agent pane starts expanded and a
     * single pointer could only remember one of them. Set from the config mode
     * each time a prompt is committed and toggled by a click, so the mode
     * decides the default and the user overrides it per pane per prompt. */
    bool  prompt_expanded;
    /* The card's two animations, per pane because the card is.
     *
     * `prompt_in`/`prompt_out`/`prompt_card_open` drive APPEARING — the shared
     * modal_anim_progress every floating surface in the app uses, so the card
     * arrives on the same curve as the palette and the voice HUD.
     *
     * `prompt_grow` drives the chip BECOMING the panel. It is a separate
     * animation because it is a different event: the card is not leaving and
     * coming back, it is one object changing shape, and crossfading two modals
     * would say the opposite. `prompt_grow_from` is the expansion the current
     * transition started at, which is what keeps a click during the grow from
     * snapping to the other end. */
    Anim  prompt_in, prompt_out;
    bool  prompt_card_open;
    Anim  prompt_grow;
    f32   prompt_grow_from;

    /* Output-burst heuristic (see TAB_ACCENT_* in ui.c), per pane: decaying
     * count of produced content plus the "agent is working" hold window. */
    f64   out_accum;
    f64   out_accum_t;
    u64   content_seen;      /* last Terminal.content_writes folded in */
    f64   work_until;
    f64   work_since;
    bool  was_working;       /* previous poll's gate — drives the done edge */
    f64   done_cooldown;     /* wall clock before another done notice may fire */

    /* Hook-driven lifecycle. When the agent's own hook reports its turn edges
     * (notify/agent_signal.h) that report OWNS work_until/work_since for this
     * pane and the output heuristic stops touching them: output volume cannot
     * tell a build log from a thinking model, so the heuristic alone would read
     * the end of every burst as a finished turn and fire the done panel plus
     * its sound.
     *
     * `hook_driven` latches on the pane's first signal and clears when the pane
     * loses its agent. Only a hook-driven pane may open the done panel; panes
     * running a tool with no hook integration still animate from the heuristic
     * but never announce, since "it went quiet" is not "it finished". */
    bool  hook_driven;
    f64   hook_started;      /* wall clock of the last START, 0 when idle */
    /* Wall clock of the last hook signal that named this pane, of any kind; 0
     * when none. It OUTLIVES the agent (the "lost its agent" reset leaves it):
     * the shell's "command finished" for an agent that has just exited reads
     * it, so the hook's own banner is not followed by a second one. */
    f64   hook_seen;
    /* An open turn that has gone dark is lit again by activity that
     * CONTINUES, not by one repaint: the first change seen while dark, 0 when
     * none is pending. See agent_busy_hook_change(). */
    f64   hook_probe;
} PaneAgent;

/* Tab content kind. The default value (0) represents a terminal backed
 * by a PTY. `TAB_FILEBROWSER` holds a `FileBrowser` and leaves the
 * terminal and session fields zeroed. `TAB_BROWSER` holds an in-app
 * loopback-preview `WKWebView`. */
typedef enum {
    TAB_TERMINAL = 0,
    TAB_FILEBROWSER,
    TAB_BROWSER,
    TAB_SFTP,            /* local | host file transfer (ui/sftp_tab.h) */
} TabKind;

/* The SFTP tab's state (ui/sftp_tab.h); only a TAB_SFTP tab has one. */
struct SftpTab;

/* Which browser the explorer's file operations act on: the docked explorer
 * (id 0), or a pane of an SFTP tab — named by the tab's id, never by a
 * pointer, because the work it starts can outlive the tab. */
typedef struct { u64 id; i32 pane; } SftpActor;

typedef struct {
    char       title[128];
    char       custom_title[128]; /* user-set title (empty = use auto title) */
    bool       title_locked;      /* if true, OSC title changes don't override */
    /* Chat title the foreground agent generated for this session, read from
     * the tool's on-disk store (Claude live-session registry, Codex session
     * index) by chat_live_title(). Runtime-only: never persisted and cleared
     * when the agent exits, so a session restore can't pin a dead chat's
     * name onto a fresh shell. Preferred by tab_effective_title() after
     * custom_title. */
    char       agent_chat_title[96];
    /* Per-pane agent state and the aggregates derived from it every poll.
     * `multi_agent_title` is non-empty only while 2+ panes run an agent — it
     * deliberately carries no tool names ("3 Agents working"), because a tab
     * strip cell is far too narrow to list them and the popup under the tab
     * is where the individual agents are spelled out. */
    PaneAgent  pane_agents[MAX_SPLIT_PANES];
    char       multi_agent_title[48];
    i32        agent_count;         /* panes with a known agent CLI */
    i32        agent_working_count; /* of those, currently producing output */
    /* Last foreground CLI agent detected for this tab (a ChatTool value,
     * stored as i32 to avoid pulling history/event.h into this header).
     * Drives auto-naming the tab after the running agent and detecting when
     * it exits. CHAT_TOOL_UNKNOWN (0) = no agent. */
    i32        last_fg_tool;
    TabKind    kind;              /* TAB_TERMINAL / TAB_FILEBROWSER / TAB_BROWSER / TAB_SFTP */
    /* SFTP tab payload (only when kind == TAB_SFTP): heap, freed by
     * app_close_tab. Its connection is `session` below. */
    struct SftpTab *sftp;
    /* File-browser tab payload (only when kind == TAB_FILEBROWSER).
     * Heap-allocated so it does not bloat every terminal tab by ~4KB;
     * freed by app_close_tab. The viewer state (view_mode, view_content,
     * …) lives inside FileBrowser already. */
    FileBrowser *fb;
    /* When the FB tab has a file open, the right-side viewer takes this
     * fraction of the tab content area. Stored per-tab so each FB tab
     * remembers its own divider position. 0 = use VIEWER_WIDTH_RATIO. */
    f32        fb_viewer_ratio;
    bool       fb_viewer_only;   /* true for detached Markdown viewer tabs */
    /* Browser payload: full-tab when kind == TAB_BROWSER, or embedded in a
     * terminal split when kind == TAB_TERMINAL and browser_pane >= 0. */
    BrowserTab *browser;
    i32        browser_pane;   /* -1 = none/full-tab; else pane index (split) */
    Terminal  *terminal;
    Session   *session;
    bool       active;
    bool       dirty;
    bool       history_tracked;  /* true once connection result has been recorded */
    bool       has_bell;       /* pending bell on this tab (cleared on focus) */

    /* Tab group membership (-1 = no group, 0+ = group index) */
    i32        group_index;

    /* Split pane */
    SplitType  split;
    f32        split_ratio;    /* 0.0-1.0, divider position */
    bool       split_move_animating;
    f64        split_move_anim_start;
    f64        split_move_anim_dur;   /* a split opening and a dropped pane take different times */
    SplitAnimRect split_move_from[MAX_SPLIT_PANES];
    /* Kept as -1 sentinels for session/state paths that still clear them;
     * pane swaps use the split_move_from[] geometry animation. */
    i32        swap_flash_slot_a;
    i32        swap_flash_slot_b;
    f64        swap_flash_start_sec;
    Terminal  *terminal2;      /* second pane terminal */
    Session   *session2;       /* second pane session */
    i32        split_pane_count;
    i32        split_root;
    i32        split_next_node;
    SplitLayoutNode split_nodes[MAX_SPLIT_NODES];
    Terminal  *extra_terminals[MAX_SPLIT_PANES - 2];
    Session   *extra_sessions[MAX_SPLIT_PANES - 2];
    i32        active_pane;    /* 0 = first, 1 = second */

    /* Terminal profile override (-1 = use global config) */
    i32        profile_index;

    /* Set when a layout pass ran while an interactive drag was deferring the
     * off-screen work (see AppState.layout_defer_inactive). The grid this tab's
     * terminals carry is one geometry behind; app_flush_pending_resizes()
     * reconciles it when the tab becomes active. */
    bool       resize_pending;

    /* Sleep state for inactive tabs */
    bool       sleeping;
    void      *sleep_prepare;      /* UI-owned incremental quiescence state */
    void      *sleep_io;           /* owned, detached snapshot job */
    i32        sleep_io_panes[MAX_SPLIT_PANES]; /* dense job slot -> physical pane */
    u32        wake_tail_tokens[MAX_SPLIT_PANES];
    Session   *wake_tails[MAX_SPLIT_PANES]; /* owned old sessions awaiting bounded final drain */
    bool       wake_adopt_ready;
    bool       wake_requested;
    WakeInputQueue wake_input;    /* allocated only while input awaits restore */
    bool       sleep_disabled;    /* user opt-out: never auto-sleep this tab */
    f64        last_activity_time;
    /* app_poll_sessions asks session_is_alive() (a waitpid()) at most every
     * 100 ms per tab instead of every iteration: a flooding pane turns the
     * loop over tens of thousands of times a second, and one syscall per tab
     * per turn adds up. Nothing reads the answer faster than the "[closed]"
     * title. */
    f64        alive_check_at;
    f64        exit_check_at;     /* last waitpid() for this tab's local shells (ui/shell_exit.h) */

    /* Timestamp of the last keystroke sent to this tab's PTY session(s). Lets the
     * agent accent-bar gate tell keystroke echo / TUI repaint (lands within a few
     * ms) apart from genuine agent output (arrives with latency) — the bar only
     * lights on output that lands >~150 ms after a keystroke. */
    f64        last_keystroke_time;

    /* Decaying count of recently produced content (≈ over the last ~0.5 s,
     * exponentially weighted). Distinguishes an agent that is actively working
     * — a sustained burst of output — from one sitting idle at a prompt, whose
     * only output is a small periodic cursor blink. Drives the tab accent
     * animation; see TAB_ACCENT_WORK_THRESHOLD in ui.c.
     *
     * AGGREGATE: the max across pane_agents[], so a split tab's bar tracks
     * whichever pane is busiest instead of pane 0 alone. The per-pane
     * accumulators (and the content_writes cursors that feed them) live in
     * PaneAgent — this field is derived, never accumulated into directly. */
    f64        out_accum;
    /* Wall-clock deadline until which the agent accent bar stays "working".
     * Refreshed whenever out_accum crosses the work threshold; the render gate
     * checks `now < agent_work_until` instead of the raw threshold so bursty
     * agent output (token gaps, thinking pauses, tool calls) doesn't blink the
     * bar on/off. See TAB_ACCENT_WORK_HOLD in ui/agent_busy.h. */
    f64        agent_work_until;
    /* Wall-clock instant the current working stretch began (rising edge of the
     * gate above). Anchors the accent bar's fade/grow-in so it eases in instead
     * of popping. Stays put while the stretch continues. */
    f64        agent_work_since;
    /* How far the working indicator's glow has travelled (points) and its
     * smoothed speed (points/sec). Integrated rather than derived from the
     * wall clock because the speed tracks output throughput — recomputing
     * `fmod(now, period)` after a speed change would teleport the glow. Every
     * place the tab is drawn folds the one distance onto its own track. */
    f32        accent_phase;
    f32        accent_speed;
    f64        accent_phase_t;
    TabSleepSnapshot *sleep_pane1;
    TabSleepSnapshot *sleep_pane2;
    /* Sleep snapshots for the 3rd+ panes of split-tree tabs. Without these,
     * sleeping a 3+-pane tab left the extra terminals (and their scrollback,
     * up to the per-terminal byte budget each) resident and their processes
     * running — sleep only reclaimed panes 1 and 2. */
    TabSleepSnapshot *sleep_extra[MAX_SPLIT_PANES - 2];

    /* per-terminal persistent render cache */
    TermRenderCache cache1;
    TermRenderCache cache2;
    TermRenderCache extra_caches[MAX_SPLIT_PANES - 2];

    /* Git status cache — populated asynchronously by git_status.c for
     * local sessions when the terminal's cwd is inside a git repo. This is
     * exactly what the status bar's git segment paints: "branch +N -M ^K". */
    struct {
        bool   valid;           /* has been polled at least once */
        bool   is_repo;
        i32    added;           /* line additions vs HEAD, incl. untracked files */
        i32    removed;         /* line deletions vs HEAD */
        i32    ahead;           /* commits ahead of upstream */
        /* The figures above are a lower bound: one of git's answers outgrew
         * the capture and only its whole records were counted. */
        bool   partial;
        /* Checked-out branch; a short SHA prefixed with '@' when HEAD is
         * detached. */
        char   branch[80];
        f64    last_check_ts;   /* platform_time_sec when last polled */
        f64    tick_ts;         /* app_git_status_tick runs at 20 Hz, not per iteration */
        char  *cached_cwd;      /* heap-alloc'd cwd at time of last poll, NULL until populated */
        /* Worktree root (`rev-parse --show-toplevel`) for that cwd. This, not
         * the cwd, is the repo's identity: it is what tells a cd deeper into
         * the same project apart from focus moving to a pane in a different
         * one, and it is what the diff dock binds to. Empty when the cwd is
         * not in a worktree (or in a bare repo, where there is no toplevel). */
        char  *repo_root;       /* heap, NULL until the first successful poll */
        /* Memo for the proc-based cwd probe (session_local_cwd is a syscall
         * pair and the sampler runs every frame). Per TAB on purpose: a memo
         * keyed on a raw Session* could match a new session that malloc
         * handed a freed session's address, and the new tab would show
         * another repo's branch and ± counts. A slot inside the tab that
         * asked cannot be recycled out from under it. */
        char  *probe_cwd;       /* heap, NULL until the first probe */
        f64    probe_ts;        /* 0 = nothing probed yet */
        i32    probe_pane;      /* active_pane the memo belongs to */
        /* When the next sample is due (ui/git_poll.h): the quiet back-off and
         * what the last run cost. */
        GitPoll poll;
        f64     cost;
    } git_status;

    /* SFTP auto-navigate — track SESSION_CONNECTED edge so the sidebar can
     * jump to the remote home the first time the session comes up (or
     * after a reconnect), independent of whether the shell emits OSC 7. */
    SessionStatus prev_status;
    bool          sftp_auto_wired;

    /* Mirror for the right split pane (session2). Needed so the read-ready
     * socket watch gets registered for the right pane too — without this
     * the right pane's keystroke echo falls back to the ~8 ms poll cadence. */
    SessionStatus prev_status2;

    /* Per-tab sidebar CONTENT, so a file opened in one tab's browser (and the
     * code editor living inside it) is not on screen in every other tab.
     *
     * These fields are the tab's STOWED copy: exactly one of (tab->fb,
     * app->filebrowser) owns the state at any moment. app_switch_tab moves it
     * across, a move and not a copy, so the heap inside FileBrowser (entry
     * list, editor buffer, undo stack, decoded images) always has a single
     * owner and fb_destroy runs once.
     *
     * Whether the sidebar is OPEN, and how wide, is deliberately not here: that
     * is the window's (AppState.sidebar_visible / sidebar_width). Carried per
     * tab, a new tab born with it closed would make the bar vanish beside an
     * open sidebar while the grid (sized with the bar) stayed put and the
     * centred leftover pushed the prompt half a sidebar to the right. The
     * toolbar toggle is a window control, and the persisted state
     * (sessions.json) is per window too. */
    FileBrowser sidebar_fb;
    SidebarMode sidebar_mode;
    bool        sidebar_focused;

    /* Drag-and-drop live animation state */
    f32        render_x;
    /* Open animation: a new tab fades + slides in. anim_start'd in app_new_tab;
     * the pill render multiplies alpha + offsets x by its eased progress. */
    Anim       tab_open;
} Tab;


/* =========================================================================
 * Recently closed tab (for undo close)
 * ========================================================================= */

#define MAX_CLOSED_TABS 8

typedef struct {
    char        title[128];
    SessionType type;           /* SESSION_LOCAL or SESSION_SSH */
    SSHConfig  *ssh_config;     /* heap copy for reconnection (SSH only) */
    char       *cwd;            /* heap-allocated CWD (local only); NULL otherwise */
    bool        valid;
} ClosedTabInfo;

typedef struct {
    char path[512];
    char passphrase[256];
} PassphraseCacheEntry;

/* Agent History resume picker row — laid out before AppState so the picker
 * row pool can live on the heap (lazy alloc, freed on app destroy). */
#define AGENT_PICKER_ROW_CAP 32
#define SSH_HISTORY_CAP      10
struct AgentPickerRow {
    /* 0 = resume in the origin agent's native session
     * 1 = start a fresh session with this agent
     * 2 = view the transcript in Liu
     * 3 = hand off the transcript as Markdown when no bridge adapter is available
     * 4 = continue through `src/history/bridge`: migrate the session into the
     *     target agent's store and resume it there. The row has no command
     *     until migration succeeds; the adapter provides it in
     *     `BridgeImportResult.launch_cmd`. */
    u8   kind;
    u8   tool;          /* ChatTool enum for resume/new/continue rows */
    /* The effective BridgeImportMode for this row, resolved when it was built
     * (BRIDGE_IMPORT_UNSUPPORTED for rows the bridge is not involved in).
     * Decides two things: whether the label reads "Continue with" or "Hand off
     * to" (a briefing is not a resume and the UI must not call it one), and how
     * strongly the row's trailing icon is tinted, which is the signal that
     * these rows do NOT all deliver the same fidelity. */
    u8   bridge_mode;
    char label[64];
    /* Store a complete shell command. A context-handoff command can include
     * about 300 characters of instruction and a state-directory path;
     * truncation could leave an unterminated quote and an interactive
     * continuation prompt. BRIDGE_CMD_CAP and agent_cmd_fits enforce the
     * size limit. */
    char command[1024];
};

/* =========================================================================
 * App state
 * ========================================================================= */

typedef struct AppState {
    Tab       tabs[MAX_TABS];
    i32       tab_count;
    i32       active_tab;

    /* Recently closed tab stack (LIFO) */
    ClosedTabInfo closed_tabs[MAX_CLOSED_TABS];
    i32           closed_tab_count;

    /* Tab groups */
    TabGroup  tab_groups[MAX_TAB_GROUPS];
    i32       tab_group_count;

    /* Config & theme */
    AppConfig    config;
    KeyBindTable keybinds;
    Vault       *vault;

    /* Window dimensions */
    i32       win_width;
    i32       win_height;
    i32       fb_width;
    i32       fb_height;
    /* The scale everything is LAID OUT at, and therefore the one the font atlas
     * was rasterised at. It moves only when a font rebuild at the new scale has
     * actually succeeded — see app_handle_dpi_change. */
    f32       dpi_scale;
    /* What the DISPLAY asked for, and whether a rebuild at that scale is still
     * owed. Kept apart from dpi_scale until that rebuild succeeds; see
     * ui/dpi_refresh.h, which holds the transaction. */
    DpiRefresh dpi_refresh;

    /* Terminal grid dimensions */
    i32       grid_cols;
    i32       grid_rows;
    f32       layout_content_w, layout_content_h;
    PaneSyncGate sync_render_gate;
    /* In-band notifications this window let through (OSC 9 / 777 / 99), and
     * the words of the last one — for the driver's state and the probe. */
    u32        osc_notify_count;
    char       osc_notify_last[200];
    /* The theme's scheme when it was last applied: 0 = never, 1 = light,
     * 2 = dark. A flip is told to the panes that asked (DEC mode 2031). */
    i8         theme_scheme_dark;

    /* UI regions (in framebuffer pixels) */
    f32       tab_bar_height;
    /* Empty zone the toolbar keeps at its left edge for the macOS traffic
     * lights. TRAFFIC_LIGHT_ZONE_PT * dpi normally, TOOLBAR_EDGE_PAD_PT * dpi
     * in native fullscreen (the lights move into AppKit's auto-hiding overlay,
     * so the reservation would just be a hole). Written by app_update_layout;
     * render_toolbar and hittest.c must both start their left-edge cursor
     * here. */
    f32       titlebar_left_inset;
    /* The right-hand twin: the zone Liu's own window controls take at the
     * toolbar's right end where the window manager draws none (X11), 0 where
     * the system draws them. Written by app_update_layout; render_toolbar's
     * right-edge content ends here. */
    f32       titlebar_right_inset;
    f32       sidebar_width;
    bool      sidebar_visible;
    SidebarMode sidebar_mode;
    f32       status_bar_height;
    f32       padding;

    /* Broadcast mode */
    bool      broadcast_mode;
    bool      broadcast_overlay_active;
    bool      broadcast_targets[MAX_TABS][2];

    /* Tab drag reorder */
    bool      tab_dragging;        /* currently dragging a tab */
    i32       tab_drag_index;      /* index of tab being dragged */
    i32       tab_drag_prev_active;/* active tab just before the drag/click started (used as dst for split-drop) */
    f32       tab_drag_start_x;    /* mouse X when drag started */
    f32       tab_drag_start_y;    /* ...and Y: the drag threshold is a distance, not an x offset */
    /* Where inside its pill the tab was picked up (px from the pill's left
     * edge). The ghost keeps this offset, so a pick-up does not jump half a
     * pill to re-centre under the pointer, and the reorder is judged from the
     * pill it describes (ui/tab_drop.h, tab_reorder_pos). */
    f32       tab_drag_grab_x;
    f32       tab_drag_offset_x;   /* current mouse X offset from start */
    i32       tab_drag_target;     /* insertion target index (-1 if none) */
    bool      tab_drag_pending;    /* mouse down on tab, waiting for threshold */
    bool      tab_drag_into_split; /* cursor entered terminal area -- showing split zones */
    i32       tab_drag_split_zone; /* 0=none 1=left 2=right 3=top 4=bottom */
    /* Tab that will host the split drop, resolved once when the drag enters
     * the terminal area (app_split_drop_dst). The view switches to it for the
     * duration of the drag so the overlay previews the real destination. -1
     * while no destination is resolved. */
    i32       tab_drag_split_dst;

    /* Cached tab layout (set by render_toolbar, read by event handling) */
    f32       _tab_bar_x;         /* start X of first tab in fb pixels */
    f32       _tab_w;             /* computed BASE tab width in fb pixels */
    f32       _tab_gap;           /* computed tab gap in fb pixels */
    /* Per-tab widening, in fb pixels: what tab i needs ON TOP of _tab_w to show
     * an icon for every distinct agent running in it (0 for the usual one-agent
     * or agentless tab). A tab's slot is _tab_w + _tab_extra_w[i]. Published by
     * render_toolbar so hittest.c and the drag code measure the same slots the
     * painter drew, instead of re-deriving them from the agent state. */
    f32       _tab_extra_w[MAX_TABS];
    /* Horizontally-scrolling tab strip. Tabs hold a comfortable minimum
     * width (TAB_MIN_W_PT); when more are open than fit, the strip scrolls
     * instead of shrinking further. All set by render_toolbar, read by
     * hittest.c and the EVENT_SCROLL handler in main.c. */
    f32       _tab_scroll_x;      /* current scroll offset (content px scrolled left) */
    f32       _tab_scroll_max;    /* max scroll = max(0, content_w - viewport_w) */
    f32       _tab_view_x;        /* tab viewport left edge (fb px) */
    f32       _tab_view_w;        /* tab viewport width (clip region, fb px) */
    f32       _tab_plus_x;        /* pinned new-tab (+) button left edge (fb px) */
    /* Update-available chip at the toolbar's far right (published by
     * render_toolbar for hittest.c; w == 0 → hidden this frame). */
    f32       _upd_btn_x;
    f32       _upd_btn_w;
    /* Broadcast indicator chip, left of the update chip (same protocol;
     * shown only while broadcast is on and the status bar is hidden). */
    f32       _bcast_btn_x;
    f32       _bcast_btn_w;
    f32       _sec_btn_x;                  /* the secure-input chip, as last drawn (w == 0: not shown) */
    f32       _sec_btn_w;
    /* Why Secure Event Input is held right now (SecureInputWhy; 0 = it is
     * not) — set by the loop for the window that has the keyboard, read by
     * the toolbar, which draws the chip. */
    u8        secure_input_why;
    /* Git segment in the STATUS BAR (not the toolbar), published by
     * render_status_bar for hittest.c; w == 0 → not painted this frame.
     * Clicking it opens the diff dock (ui/git_diff.c). `_git_btn_hovered`
     * only exists so the main loop can repaint on enter/leave — the paint
     * itself re-derives hover from hover_x/y, which is a frame fresher. */
    f32       _git_btn_x;
    f32       _git_btn_w;
    bool      _git_btn_hovered;
    GitDiffView git_diff;
    i32       _tab_scroll_last_active; /* active tab we last auto-scrolled to */
    /* Per-tab painted pill rect, published by render_toolbar for anything that
     * has to point AT a tab rather than hit-test one (the tab popup anchors
     * its panel here). w == 0 → that tab wasn't painted this frame (collapsed
     * group / scrolled out of the strip). Indexed by tab index. */
    f32       _tab_pill_x[MAX_TABS];
    f32       _tab_pill_w[MAX_TABS];
    /* The docked (sidebar) tab list publishes its rows too: y/h per tab, and
     * the flag that says which of the two painters wrote them this frame. */
    f32       _tab_pill_y[MAX_TABS];
    f32       _tab_pill_h[MAX_TABS];
    bool      _tab_pills_in_sidebar;
    /* Docked mode's "+" in the top bar (w == 0 when not painted). */
    f32       _dock_new_btn_x, _dock_new_btn_w;
    /* Hover preview / agent-done notice for THIS window (ui/tab_popup.c). */
    TabPopup  tab_popup;
    /* Tab under the pointer this frame, -1 = none. Same source of truth the
     * pill hover fill uses, so the popup can never disagree with the visual. */
    i32       _tab_hover_index;

    /* Bell */
    f64       bell_flash_time; /* timestamp of last bell flash (0 = inactive) */
    bool      window_focused;  /* is the app window currently focused? */

    /* Sidebar resize drag */
    bool      sidebar_resizing;

    /* Set for the duration of an interactive drag that re-derives the grid on every mouse-move
     * (sidebar divider, window edge). While it is on, app_update_layout only reflows the tab the
     * user can actually see and marks the rest Tab.resize_pending; activation reconciles hidden
     * tabs. Non-interactive layout changes use the same lazy hidden-tab policy. Reflow is
     * O(scrollback) per terminal and the off-screen tabs cannot show the result mid-drag, so
     * reflowing all of them per step would spend the frame budget and send a SIGWINCH storm to
     * every background shell. */
    bool      layout_defer_inactive;

    /* Right-edge reserve (git dock + file viewer) the grid was last laid out
     * against. The viewer is opened, closed, detached and dragged from a dozen
     * places; comparing the reserve once per frame is what keeps the grid in
     * step with all of them without a relayout call at every one. */
    f32       layout_right_reserve;
    /* Sidebar width the grid was last laid out against, for the same reason:
     * the width can settle somewhere no handler laid the grid out for — the
     * docked bar's auto-fold to its rail below sidebar_collapse_below, a
     * session restore, LIU_SIDEBAR_DEMO — and a grid sized for the wrong bar
     * draws its centred leftover as a blank band beside the text. Compared once
     * per frame in app_render, only while the slide is at rest, so the terminal
     * is never resized per animation frame. */
    f32       layout_sidebar_w;

    /* File viewer (code editor) resize drag */
    f32       viewer_width;   /* current width in fb pixels; 0 = use VIEWER_WIDTH_RATIO default */
    bool      viewer_resizing;
    /* Detach drag: mouse-down on viewer title, drag onto tab bar → new tab */
    bool      viewer_drag_pending;
    bool      viewer_dragging;
    f32       viewer_drag_start_x, viewer_drag_start_y;
    FileBrowser *viewer_drag_source;
    char      viewer_drag_path[FB_MAX_PATH * 2];
    /* Markdown viewer whose outline drives the palette "Go to Heading" mode;
     * set when that mode is entered, consumed when a heading is activated. */
    FileBrowser *outline_fb;
    f32       hover_x;
    f32       hover_y;

    /* Smooth cursor animation */
    f32       cursor_anim_x, cursor_anim_y;            /* current visual position (pixels) */
    f32       cursor_anim_from_x, cursor_anim_from_y;  /* lerp start — snapshot at retarget */
    f32       cursor_target_x, cursor_target_y;        /* target position (pixels) */
    f64       cursor_anim_start;                       /* animation start time */
    bool      cursor_animating;                        /* true while interpolation in progress */
    Terminal *cursor_anim_term;                        /* terminal that owns the current
                                                        * animation state; switching panes
                                                        * (new term) snaps instead of
                                                        * animating across the divider */

    /* Floating notices (ui/toast_stack.h). `toast_message` and
     * `toast_start_time` are the NEWEST one's words and clock — kept for the
     * state dump and for the policies that only ask "is one up" — and never
     * what the loop's gates wait on: those ask `toasts`. */
    ToastStack toasts;
    char      toast_message[256];
    f64       toast_start_time;  /* 0 = inactive */

    /* Previous-run crash banner (populated on launch from crashlog). Persists
     * at the top of the window until the user dismisses it (Esc / click). */
    bool      crash_banner_active;
    char      crash_banner_reason[256];
    char      crash_banner_path[512];
    f32       crash_banner_x, crash_banner_y, crash_banner_w, crash_banner_h; /* last drawn rect (hit-test) */
    /* A notice that remains until dismissed when important status should
     * outlast the three-second toast, such as a change to agent hook
     * configuration. It shares the crash banner's dismissal behavior and
     * neutral colors, and is drawn below it when both are visible. */
    bool      info_banner_active;
    char      info_banner_line1[200];
    char      info_banner_line2[200];
    f32       info_banner_x, info_banner_y, info_banner_w, info_banner_h;

    /* Translate-on-Tab: Ctrl+Cmd×2 within cfg.translate.tab_window_sec triggers a
     * prompt-line translation while the foreground process is an allowlisted
     * agent CLI. The pending target is pinned to the pane that saw the first
     * chord so focus changes cannot trigger translation elsewhere. */
    f64       translate_tab_first_time;
    f64       translate_tab_last_time;
    i32       translate_tab_pending;
    Terminal *translate_tab_target_term;
    Session  *translate_tab_target_sess;
    bool      translate_chord_down;  /* physical Ctrl+Cmd combo is currently held */
    /* The chord was pressed while the agent had not yet DRAWN what was just
     * typed (app_translate_screen_behind): the prompt is read when it has, or
     * when this time is reached — and not at all if a key reaches the pane
     * meanwhile (`_serial`). 0 = nothing is waiting. */
    f64       translate_settle_until;
    Terminal *translate_settle_term;
    Session  *translate_settle_sess;
    u64       translate_settle_serial;
    char      translate_fg_agent_id[AGENT_ID_CAP]; /* agent that owns the prompt */
    /* Agent the in-flight run is actually going through — the Settings pick
     * normally, the foreground agent only when that pick failed to spawn.
     * Every toast and failure log reads this so the name shown is the CLI that
     * really did the work. Empty until a spawn succeeds. */
    char      translate_run_agent_id[AGENT_ID_CAP];
    char      translate_input_shadow[4096];
    i32       translate_input_shadow_len;
    Terminal *translate_input_shadow_term;
    Session  *translate_input_shadow_sess;
    f64       translate_input_shadow_time;
    bool      translate_input_shadow_overflow; /* shadow lost bytes — capture
                                                * must fall back to the grid */
    /* The rest of the mirror (ui/input_mirror.h): where the next character
     * goes in, where the line being typed starts, and the session it is of
     * (session_instance_id — the pointers above are only ever compared).
     * `unsure`: a key has put the program's caret somewhere the mirror cannot
     * follow, or may have rewritten the text. What it holds may still be read
     * by whoever checks it against the cells; nothing more is put into it.
     * `loose`: it holds a paste, which the program may be showing as a label
     * instead — still kept up, but no longer the prompt byte for byte. */
    i32       translate_input_shadow_caret;
    i32       translate_input_shadow_line;
    u64       translate_input_shadow_owner;
    bool      translate_input_shadow_unsure;
    bool      translate_input_shadow_loose;
    bool      translate_active;
    TranslateDraft *translate_draft;
    /* The prompt's look while it is translated (ui/translate_fx.h): the
     * effect's state, the pane it is in (compared, never dereferenced), the
     * cells of the prompt as the last loop turn found them, and the rows it
     * painted last — repainted once more, in their own colours, when it ends. */
    TranslateFx     translate_fx;
    Terminal       *translate_fx_term;
    TranslateFxSpan translate_fx_span;
    bool            translate_fx_span_ok;
    bool            translate_fx_restore;
    i32             translate_fx_row0, translate_fx_row1;
    TranslateFxPaint translate_fx_painted;   /* what those rows were last built with */
    TranslateConfig translate_config_snapshot;
    u64       translate_input_serial;
    i32       translate_fg_pid;
    /* Route a completed dictation through the existing Translate flow. Buffer
     * the transcript and pass it through app_voice_clean_text so a carriage
     * return cannot submit a prompt; the auto-submit setting controls
     * submission. No initial backspace sequence is needed because the text was
     * not typed into the pane. */
    bool      translate_from_voice;
    char      translate_voice_out[8192];
    i32       translate_voice_out_len;
    bool      translate_cancel_requested;
    bool      translate_local_inflight; /* true: local-LLM backend in flight */
    bool      translate_api_inflight;   /* true: API (curl) backend in flight —
                                         * shares child_pid/stdout_fd with the
                                         * agent backend, but the response is
                                         * JSON so the tick finalizes instead
                                         * of streaming raw child bytes */
    char      translate_api_provider[16]; /* provider snapshot at API spawn;
                                           * finalize + toasts read this, not
                                           * live config, so a mid-flight
                                           * Settings change can't flip the
                                           * parse shape under the response */
    bool      translate_write_started;  /* local backend: streamed bytes started */
    char      translate_typed[4096];    /* captured prompt-line text to translate
                                         * (>= translate_input_shadow so the
                                         * capture is never truncated) */
    usize     translate_typed_from;     /* from this byte on it was typed here
                                         * (cmdline_capture_prompt_ex) */
    i32       translate_capture_kind;   /* CmdCaptureKind of translate_typed: EXACT
                                         * (an erase may be counted from it) or
                                         * WHOLE (read from the cells; only an
                                         * erase that empties the input) */
    char      translate_stream_line[1024];
    i32       translate_stream_line_len;
    bool      translate_stream_ready;
    bool      translate_stream_emitted;
    /* Agent-backend progress notifications: whether "receiving from
     * <agent>" has already been announced, so the toast fires once per
     * translation when the child's first stdout byte arrives. */
    bool      translate_agent_first_bytes_announced;
    i32       translate_child_pid;      /* agent backend: fork()ed child */
    i32       translate_stdout_fd;      /* agent backend: pipe read end; -1 idle */
    /* Set when the agent writes its answer to a file instead of stdout (codex —
     * see TranslateAgentSpawn.last_message_path). While non-empty the child's
     * stdout is logged but never streamed into the prompt: it is banner and
     * diagnostics, not the translation. Read + unlinked on child exit. */
    char      translate_last_msg_path[256];
    f64       translate_spawn_time;
    PollGate  translate_poll;          /* the agent poll's own 10 Hz clock (core/poll_gate.h) */
    /* Transcript cleanup for grok/opencode (no prevent-flag exists): session
     * store snapshot + cwd taken at spawn, used by translate_cleanup_after_exit
     * to delete only the new translate one-shot session. */
    TranslateCleanupSnap translate_cleanup_snap;
    char      translate_cleanup_cwd[1024];
    char      translate_cleanup_agent[AGENT_ID_CAP]; /* agent the snapshot was taken for */
    char     *translate_log;
    i32       translate_log_len;
    i32       translate_log_cap;
    Terminal *translate_target_term;
    Session  *translate_target_sess;
    /* Prompt translations erase TEXT after backend acceptance, then write
     * validated output as it arrives. KEEP/CHIP stay in place. Voice uses 0. */
    TranslateSeg translate_segs[TRANSLATE_MAX_SEGS];
    i32       translate_seg_count;
    i32       translate_seg_cur;
    bool      translate_seg_text_emitted; /* current TEXT seg wrote bytes */
    char      translate_seg_scratch[4096];/* NUL-terminated current TEXT seg */

    /* Path of the background image currently uploaded to the GPU. Compared
     * against config.background_image so re-applying settings is free unless
     * the picture actually changed — a reload is a disk read plus a texture
     * upload, and app_apply_window_background() runs on every config apply,
     * theme switch and profile change. */
    char      bg_image_loaded[512];
    char      bg_image_requested[512];
    /* Wallpaper decode. The job itself is process-wide and shared with the
     * other windows (ui/bg_image.h) — every window showing one picture costs
     * one stb_image decode, not one per window — and this holds only a
     * subscription to it, so a window can close mid-decode without waiting. */
    struct BgImageJob *bg_job;

    /* Scrollbar: the auto-hide stamp (only read with config.scrollbar_autohide)
     * and the thumb drag. `scrollbar_drag_term` is COMPARED, never
     * dereferenced — a pane can close under a held button; `scrollbar_drag_pane`
     * is the pane slot it was grabbed in (every pane has its own bar), checked
     * against the terminal that slot holds NOW before each move. */
    f64       scrollbar_last_activity;
    bool      scrollbar_dragging;
    f32       scrollbar_drag_grab;     /* pointer y minus thumb top at the press */
    const Terminal *scrollbar_drag_term;
    i32       scrollbar_drag_pane;

    /* Scroll-to-bottom buttons — published by the renderer once per frame,
     * one entry per visible terminal pane that is currently scrolled up.
     * The hit test and the click handler read this instead of re-deriving
     * the pane geometry, so the painted button and its click target can
     * never drift apart. Cleared at the top of every render_terminal(). */
    struct {
        Terminal *term;          /* pane this button scrolls to the bottom */
        f32       x, y, w, h;
    }         scroll_btn[MAX_SPLIT_PANES];
    i32       scroll_btn_count;

    /* Image-file badges — the same arrangement as scroll_btn above, and for
     * the same reason: the renderer is the only thing that knows where a row
     * landed after scroll, split and BiDi, so it publishes the rect and the
     * click handler reads it rather than re-deriving geometry that could drift.
     * `row` is the VISIBLE row, re-scanned on click to recover the path — one
     * row's worth of work on a click is free, and it keeps this table small
     * enough to live in every window's AppState.
     * Cleared at the top of every render_terminal(). */
    struct {
        Terminal *term;
        i32       row;
        f32       x, y, w, h;
    }         img_badge[MAX_IMG_BADGES];
    i32       img_badge_count;
    /* The overlay a badge click opens. Per window — see lightbox.h. */
    Lightbox  lightbox;
    Sketch    sketch;
    /* Rows that wanted a badge past the cap, so a full screen of image paths
     * reports the truncation instead of silently showing fewer. */
    i32       img_badge_dropped;

    /* Prompt-recall badges — one per agent pane drawn this frame, published by
     * the renderer and read by the click handler, exactly like img_badge above
     * and for the same reason (the renderer is the only thing that knows where
     * a pane landed). `term` is only ever COMPARED, never dereferenced: a pane
     * can close between the frame that published the rect and the click.
     * Cleared at the top of every render_terminal(). */
    struct {
        Terminal *term;
        f32       x, y, w, h;
    }         prompt_badge[MAX_SPLIT_PANES * 2];
    i32       prompt_badge_count;

    /* Attachment tiles inside an EXPANDED prompt panel, so a click on one opens
     * the lightbox instead of collapsing the card. Published on the same
     * contract as prompt_badge (rebuilt from scratch each frame, `term` only
     * ever compared) and tested BEFORE it, because the tile sits on top of the
     * panel that would otherwise swallow the click.
     *
     * Only published while the card is at REST: a rect that is still sliding is
     * not one the user aimed at, and the tiles carry hover fades whose slots are
     * keyed on the rect they are handed. `img` indexes PaneAgent.prompt.img,
     * which is re-read at click time — the path is never cached here. */
    struct {
        Terminal *term;
        i32       img;
        f32       x, y, w, h;
    }         prompt_thumb[MAX_SPLIT_PANES * PROMPT_RECALL_MAX_IMGS];
    i32       prompt_thumb_count;

    /* The multi-line prompt currently being COMPOSED, staged away from the
     * pane's committed one.
     *
     * prompt_recall_line resets the buffer when it is handed a line for a
     * prompt that is not open yet. Composing into the committed prompt would
     * therefore destroy what the chip shows at the first Shift+Enter, leave the
     * chip blank for good after a Ctrl+C (only a plain Enter or the next turn's
     * rising edge closes it), and merge an abandoned prompt into the next one
     * ("aaa" + Ctrl+C + "bbb" shown as "aaa bbb"). Staged, the committed prompt
     * stays untouched until a new one actually commits, and an abandoned
     * compose is simply dropped.
     *
     * ONE buffer per window rather than one per pane, because only the focused
     * pane can be typed into: `prompt_compose_pa` says whose it is, and is only
     * ever COMPARED, never dereferenced (a pane can close mid-compose). */
    PromptRecall prompt_compose;
    PaneAgent   *prompt_compose_pa;

    /* Short interaction boost window for adaptive frame pacing */
    f64       interaction_boost_until;
    /* Wall-clock time of the last keyboard input event (key-down / char input).
     * Input-only: NOT armed by mouse or by terminal output, so it cleanly marks
     * "the user is actively typing" with no false positives from visible
     * streaming output. The main loop drops vsync while a keystroke is within
     * TYPING_VSYNC_WINDOW_SEC of now, so keystroke echo presents at render pace
     * instead of stalling on ProMotion's idle refresh boundary. */
    f64       last_key_time;
    /* Set by app_poll_sessions when data was fed into the *visible*
     * terminal (active tab, focused pane). Background-tab keepalives or
     * chatter from non-visible panes intentionally do not flip this, so
     * remote output nobody sees does not hold ProMotion at 120 Hz. */
    bool      visible_session_data_this_frame;
    bool      poll_ui_changed; /* asynchronous metadata changed this window's chrome */
    /* A local shell exited and its pane is to be closed: found by the session
     * poll, carried out by main.c right after it (closing a pane shifts the
     * indices the poll is walking). One at a time; the next is found on the
     * next pass. */
    i32       updater_seen_phase;   /* the updater phase the last tick saw (an install failure toasts on the edge) */
    bool      shell_exit_pending;
    i32       shell_exit_tab, shell_exit_pane;

    /* Panes that produced data on the PREVIOUS poll. The read budget is shared
     * across panes (see core/poll_budget.h) and the current frame's count is
     * not known until it has already been spent, so last frame's stands in —
     * it is a good predictor, and being one frame late only matters on the
     * frame a flood starts. */
    i32       poll_busy_panes;
    /* Counts panes that used their full read budget on the previous poll;
     * visible panes are counted separately. */
    i32       poll_saturated_panes;
    i32       poll_saturated_visible;
    /* The MEM_RENDER_CPU accounting walk (every pane's row caches, O(rows)
     * per cache) is re-derived at most every 250 ms; between walks the last
     * sum is reported. It feeds a 1.5 GiB soft ceiling, which cannot tell a
     * quarter of a second of staleness from none. */
    f64       render_cpu_walk_at;
    usize     render_cpu_walk_bytes;
    /* Of those, the ones whose drain reads the kernel inside the poll loop
     * rather than a ring a background thread already filled. See
     * poll_hold_if_saturated. */
    i32       poll_saturated_watch;
    /* Terminal rows actually rebuilt during the last app_render. Zero means
     * the frame re-emitted an unchanged grid — the case a frame-level "nothing
     * changed" gate would skip. Observation only: the gate itself is not in
     * (see the note in main.c's probe), because the same predicate has to
     * account for every non-grid surface too. */
    /* Last atlas PLACEMENT generation this window's row caches were built
     * against. A shelf reclaim or a colour-atlas reset moves packed glyphs
     * to new texel positions, and the caches hold those rects baked into
     * GPU instances — see the blanket rebuild in app_render. (A grow keeps
     * pixels in place and does not bump it.) */
    u32       atlas_layout_seen;
    bool      atlas_probe_skip_preparation; /* When true, skip atlas preparation for this window's frame. */
    /* Last atlas GPU-ready version seen by this window's app_render. It
     * advances inside renderer_begin_frame when the frame's preparation
     * queues the texels written since the last one; a row that drew a
     * placeholder for a glyph whose texels were not yet queued is repaired
     * when this moves. */
    u32       atlas_ready_seen;
    /* Counts frames produced by this window. Deferred renderer_begin_frame
     * calls do not increment the count. */
    u64       frames_produced;

    i32       frame_rows_rebuilt;
    /* The other half of that question, and the reason "0 rows rebuilt" is not
     * the whole story: a frame that rebuilds nothing still REPLAYS every
     * cached row into the renderer's batches. These count what that replay
     * costs, so "does a static pane cost anything" has a number rather than an
     * argument.
     *
     *   rows_replayed   rows whose cached instances were appended unchanged
     *   bg/glyph/deco   instances copied out of the caches this frame
     *   replay_ns       CPU nanoseconds inside the append loops
     *   rebuild_ns      CPU nanoseconds inside term_render_cache_build_row
     *
     * All are reset per frame by app_render and are observation only. */
    i32       frame_rows_replayed;
    i32       frame_bg_instances;
    i32       frame_glyph_instances;
    i32       frame_deco_instances;
    /* The two the benchmark HUD's GRID group needs and the pair above cannot
     * supply: the cells inside the rebuilt rows (rows x width is only equal
     * to it while every rebuild walks a full row), and the glyph instances
     * those rebuilds produced — frame_glyph_instances counts the replay,
     * which emits the cached rows too, so on a static pane it is large while
     * nothing was shaped at all. */
    i32       frame_cells_rebuilt;
    i32       frame_glyphs_shaped;
    u64       frame_replay_ns;
    u64       frame_rebuild_ns;
    u64       prepare_submitted, prepare_adopted, prepare_discarded, prepare_expired;
    /* Bumped once per app_render. Without it a reader outside the render path
     * cannot tell "this frame rebuilt 30 rows" from "no frame was painted and
     * the counters still hold the last one's", as for a window the occlusion
     * gate is skipping entirely. */
    u64       frame_seq;

    /* Overlays */
    SettingsPanel settings;

    /* Sites / dev-server manager: backend runtime + UI view state. */
    SiteManager   site_mgr;
    SiteManagerUI sites;
    /* app_browser_push_recent changed the browser's Recents; the loop saves
     * them and hands them to the other windows (liu_browser_recents_save). */
    bool          browser_recents_dirty;
    /* A panel outside main.c changed this window's config (the sidebar's
     * Customize popover) or its Site Manager list: the loop saves it and
     * hands it to the other windows (liu_config_commit). Every window keeps
     * a whole copy, and a stale one written later would take the change
     * back. */
    bool          config_commit_pending;
    bool          sites_commit_pending;

    /* In-app auto-updater (checks a feed, verifies + installs new versions). */
    UpdateState   updater;

    /* Host OS window — used by TAB_BROWSER to attach WKWebView. Set once
     * from main after platform_window_create; not owned by AppState. */
    struct PlatformWindow *host_window;

    /* Tab close animation: a ghost pill (snapshot of the just-closed tab) that
     * shrinks + fades out in place while the remaining tabs slide into the gap.
     * The real tab is removed from tabs[] synchronously; this is purely the
     * visual exit, drawn as an overlay after the live pills. */
    bool          tab_close_active;
    Anim          tab_close_anim;
    f32           tab_close_x;       /* slot left edge (render_x) at close time */
    f32           tab_close_w;       /* tab slot width at close time */
    bool          tab_close_was_active;
    bool          tab_close_in_group;
    Color         tab_close_group_col;

    /* Tab rename */
    bool      tab_rename_active;
    i32       tab_rename_index;
    char      tab_rename_buf[128];
    i32       tab_rename_len;
    /* When the rename began. The editor opens on the FIRST mouse-down of a
     * context-menu row click; a fast second click (double-click habit) lands
     * after the menu is gone, hits whatever sits underneath, and would commit
     * + close the editor before the user typed anything. Clicks inside this
     * grace window after the start are ignored by the commit-on-outside-click
     * rule (chip rename shares the same guard via chip_rename_started_at). */
    f64       tab_rename_started_at;

    /* Tab context menu (right-click on a tab) */
    bool      tab_ctx_menu_active;
    i32       tab_ctx_menu_tab_index;
    f32       tab_ctx_menu_x;
    f32       tab_ctx_menu_y;
    i32       tab_ctx_menu_selected;   /* hovered row index, -1 = none */

    /* Terminal area context menu (right-click on the terminal pane).
     * Copy / Rename Tab / the four directional splits. */
    bool      term_ctx_menu_active;
    f32       term_ctx_menu_x;
    f32       term_ctx_menu_y;
    i32       term_ctx_menu_selected;
    /* Whether the pane had a selection when the menu OPENED. Latched rather
     * than re-read per frame: the row list must not change shape underneath a
     * click, and a stray selection_clear between the paint and the click would
     * otherwise shift every row up by one and fire the wrong action. */
    bool      term_ctx_menu_can_copy;
    /* The pane the user right-clicked, NOT the focused one. Right-clicking
     * does not move focus, so acting on the focused pane would make the
     * menu lie: Copy would take the other pane's selection and Split would
     * divide the other pane while the menu sits over this one. -1 = no
     * split (single pane). */
    i32       term_ctx_menu_pane;
    /* The link under the pointer when the menu OPENED ("" = none), latched
     * for the same reason as can_copy. A right-click on a link opens the
     * menu like a right-click anywhere else, and Open Link / Copy Link are
     * rows in it, so the gesture never replaces the clipboard by itself. */
    char      term_ctx_menu_url[2048];

    /* Tab-group chip context menu (right-click on a workspace chip) */
    bool      group_ctx_menu_active;
    i32       group_ctx_menu_group_index;
    f32       group_ctx_menu_x;
    f32       group_ctx_menu_y;
    i32       group_ctx_menu_selected;
    /* Color submenu: when selected==COLOR row, this shows palette swatches */
    bool      group_ctx_menu_color_open;

    /* Inline chip rename state (mirrors tab_rename_*) */
    bool      chip_rename_active;
    i32       chip_rename_group_index;
    char      chip_rename_buf[64];
    i32       chip_rename_len;
    f64       chip_rename_started_at;   /* double-click grace; see tab_rename_started_at */

    /* Drag-tab drop target: workspace chip group index (-1 = none).
     * Set during mouse-move while tab_dragging if hover is on a chip. */
    i32       tab_drag_target_group;
    /* The group the dragged tab was in when the drag began (-1 = none). A tab
     * dragged out of a group it was the only member of leaves that group with
     * nothing on the strip to drag back onto; this is how the same gesture can
     * still put it back. Compared against tab_groups[], never trusted blind. */
    i32       tab_drag_home_group;

    /* Agent resume picker (opened after choosing a session in Agent History).
     * Lists CLI agents installed locally and lets the user pick which one to
     * resume/launch with. Rows are baked in agent_picker_rows[], the kind
     * and target shell command live inside each row. */
    bool      agent_picker_active;
    char      agent_picker_title[96];
    char      agent_picker_session_id[96];
    char      agent_picker_session_path[512];
    char      agent_picker_session_cwd[512];   /* where it ran, "" when not recorded */
    u8        agent_picker_session_tool;
    i32       agent_picker_selected;

    /* Lazy CLI detection (one-shot per process). Indexed by ChatTool enum —
     * slot 0 is UNKNOWN and always false. Size comfortably above the current
     * CHAT_TOOL_COUNT_ so adding new agents needs no struct layout change. */
    bool      agent_detect_done;
    bool      agent_has[32];

    /* Built per picker-open. Heap-allocated on first use so the ~10 KB row
     * pool doesn't sit in BSS for users who never open the picker. */
    struct AgentPickerRow *agent_picker_rows;   /* [AGENT_PICKER_ROW_CAP] when non-NULL */
    i32       agent_picker_row_count;
    /* Rows built while an agent's version was still being asked for
     * (bridge_compat_version_peek) read as provisional; they are built again
     * when the probe generation they saw moves (app_agent_picker_tick). */
    bool      agent_picker_compat_pending;
    u32       agent_picker_compat_gen;

    /* Pending resume/start-new command waiting for the focused session's
     * foreground process group to drain (e.g. a previous Claude CLI exits
     * after SIGTERM) before the new shell command can be pasted. Cleared
     * once the prompt is back or the deadline passes. */
    i32       input_pane_override; /* scoped to replay of a buffered terminal event */
    Session  *pending_agent_session;
    u64       pending_agent_session_id;
    char      pending_agent_command[1024];  /* holds an AgentPickerRow.command verbatim */
    f64       pending_agent_deadline_sec;
    bool      pending_agent_active;
    bool      pending_agent_kill_sent;
    u32       pending_agent_probe;

    /* In-app transcript viewer modal — shown when the user picks "View
     * transcript" in the agent picker. Events are fully parsed ahead of
     * time and kept in a heap-allocated pool that lives until close. */
    bool       transcript_viewer_active;
    char       transcript_title[128];     /* "<project> · <session-id-short>" */
    u8         transcript_tool;            /* ChatTool */
    struct TranscriptEvent {
        u8     role;                       /* ChatRole */
        i64    ts_ms;
        char  *text;                       /* heap-dup; may be NULL */
        char  *tool_name;                  /* heap-dup; may be NULL */
    } *transcript_events;
    i32        transcript_count;
    i32        transcript_cap;
    i32        transcript_scroll;          /* top visible row index */

    /* ---- Shared single-line editing state (see ui/textedit.h) -------------
     * Exactly one chrome field owns the keyboard at a time — the palette
     * query, a rename box, the search bar, a dialog field — so caret,
     * selection, horizontal scroll and undo live here once instead of being
     * duplicated into every field's struct. `te_owner` is the buffer the
     * state currently belongs to; focus moving elsewhere re-anchors, and
     * `te_shadow` catches the many places that rewrite a buffer directly
     * (mode switches, prefills) so a stale caret never survives. */
    const void  *te_owner;
    i32          te_caret;      /* byte offset of the insertion point */
    i32          te_sel;        /* byte offset of the selection anchor */
    i32          te_scroll;     /* first visible character, for long values */
    i32          te_shadow;     /* owner's length as of the last edit */
    TextEditUndo te_undo;

    /* Search bar (Cmd+F) */
    bool      search_active;
    char      search_query[256];
    i32       search_query_len;
    /* The search is LIVE: re-run on every edit of the query and again when
     * the pane it runs in changes underneath it (matches are grid rows, so
     * a line of output shifts every one of them). These say which terminal
     * the current match array was built against and at what state. */
    const Terminal *search_term;
    u64       search_writes;
    i32       search_sb;
    i32       search_rows;
    /* Absolute line (sb_abs_base + sb_count + row) and column of the current
     * match, captured WHEN it was chosen. Recomputing it from the stale row
     * against the new sb_count after output arrived names the line below. */
    i64       search_cur_abs;
    i32       search_cur_col;
    bool      search_reveal_pending;
    /* The bar's two explicit modes: the "Aa" and ".*" toggles, also
     * Cmd+Option+C / Cmd+Option+R while it is open. Plain, case-insensitive
     * text is the default and searches exactly what was typed: there is no
     * implicit /pattern/ syntax, so the slashes of a path like /tmp/ are
     * searched for. Sticky for the window across open/close. */
    bool      search_case;
    bool      search_regex;

    /* Command palette (Cmd+K) */
    bool      palette_active;
    char      palette_query[128];
    i32       palette_query_len;
    i32       palette_selected;
    i32       palette_scroll;
    /* When non-zero, palette is in a numeric-input sub-mode instead of fuzzy
     * search. 1 = font size input. */
    u8        palette_input_mode;
    /* Scrollbar geometry cached by the palette renderer for the click
     * handler to hit-test against. All in framebuffer pixels; valid only
     * for the frame after the renderer ran while overflow existed. */
    f32       palette_sb_x, palette_sb_w;
    f32       palette_sb_track_y, palette_sb_track_h;
    f32       palette_sb_thumb_h;
    i32       palette_sb_scroll_max;
    bool      palette_sb_dragging;
    f32       palette_sb_grab_offset;   /* mouse_y - thumb_top at grab time */
    /* The palette's geometry as the renderer last drew it, so the pointer
     * maps (x,y) to a row with the painter's own numbers: pal_row_at() over
     * this is the ONE row test. Framebuffer pixels; valid for the frame after
     * the renderer ran while the palette was open. */
    PalLayout pal_lay;                  /* the palette as last drawn (ui/palette_layout.h) */
    i32       palette_row_count;        /* rows the filter let through that frame; 0 = none to take */

    /* SSH connect dialog */
    bool      ssh_dialog_active;
    /* The dialog opens an SFTP tab (a file-transfer connection) instead of a
     * terminal: "SFTP Connect". Kept through the close (every opener sets it),
     * so the form leaves as what it was. */
    bool      ssh_dialog_sftp;
    /* The explorer's file operations act on this browser (SftpActor): set
     * around a call made for an SFTP tab's pane, and kept with the prompt it
     * opened, so Enter in that prompt acts on the same pane. */
    SftpActor fb_actor;
    SftpActor fb_prompt_actor;
    char      ssh_host[256];
    char      ssh_user[128];
    char      ssh_port[8];
    /* ssh_port holds the form's own "22" (an SSH or SFTP form opens with
     * it), not one the user typed: the submit reads it as "not typed", so a
     * ~/.ssh/config Port still wins. The first edit ends it. */
    bool      ssh_port_auto;
    char      ssh_password[256];
    /* How many characters the password field held when the form closed: its
     * dots fade out with the form. The password itself is wiped at once. */
    i32       ssh_pass_fade_len;
    i32       ssh_field;  /* 0=host, 1=user, 2=port, 3=password */

    /* Command history popup (Option+Up) */
    bool      cmd_history_active;
    /* The timestamp of the KEY_DOWN that just ran a shortcut, on X11. The
     * backend pushes the keystroke's CHAR_INPUT after it with the SAME
     * timestamp (Ctrl+<letter> as its control code, Alt+<key> as text), so
     * Ctrl+Alt+D would split AND send ^D to the shell. 0 = none. */
    f64       shortcut_char_time;
    /* The same pairing for a keystroke whose KEY_DOWN the focused pane's
     * keyboard protocol already encoded (terminal_key_input sent bytes):
     * Ctrl+C under the keyboard protocol went out as CSI 99;5u AND as its ^C,
     * Alt+B as CSI 98;3u and ESC b. Its CHAR_INPUT twin is dropped. X11 only. */
    f64       key_text_time;
    /* Where an input method composes, in framebuffer pixels, as the last
     * render placed it: the focused pane's cursor cell, or the caret of the
     * text field that has the keyboard (ui_ime_field_caret). The candidate
     * list hangs from it and the composition is drawn on it. */
    f32         ime_spot_x, ime_spot_y, ime_spot_h;
    bool        ime_spot_valid;
    bool        ime_field_now;    /* a field claimed the spot this frame */
    bool        ime_field_prev;   /* ... the frame before: the pane draws none */
    const void *ime_field_owner;  /* that field (its buffer), as an identity */
    ImeTarget   ime_target;       /* where a composition belongs (ui/ime_target.h) */
    /* Why app_init() returned false, in words for the startup alert (the
     * renderer and the font fail for different reasons). NULL otherwise. */
    const char *init_failure;
    bool      cmd_history_loading;
    Terminal *cmd_history_target_term;
    Session  *cmd_history_target_sess;
    char      cmd_history[5][512];       /* sanitized label shown in popup */
    char    (*cmd_history_raw)[4096];    /* heap-allocated [5][4096], original command replayed on Enter */
    i32       cmd_history_count;
    i32       cmd_history_selected; /* 0 = most recent */

    /* Inline autosuggest mirrors the most recent folder-history match for
     * the current prompt prefix. It appears in dim text after the cursor;
     * Right arrow accepts it and inserts the remaining suffix into the PTY.
     */
    bool      autosuggest_visible;
    char      autosuggest_full[4096];    /* full history line that matched */
    char      autosuggest_suffix[4096];  /* the portion still un-typed */
    Terminal *autosuggest_target_term;   /* term the suggestion applies to */
    Session  *autosuggest_target_sess;

    /* True once something other than plain typing rewrote the shell's line
     * (Tab completion, history recall). The typed mirror
     * then covers only part of the input, so it must not be used to pin the
     * command line's start — the prompt scan takes over. Cleared on Enter,
     * where the next line starts empty. */
    bool      cmdline_hint_stale;

    /* Render benchmark HUD (Cmd+K -> "Toggle Render Benchmark"). Off by
     * default and costs nothing while off: the sample ring is heap-allocated
     * lazily on the first toggle and every hook is gated on bench_enabled. */
    bool      bench_enabled;
    bool startup_observe; /* one-shot first-presentation startup trace */
    f64      *bench_samples_ms;       /* ring of per-frame render ms (NULL until first toggle) */
    i32       bench_head;             /* next write index */
    i32       bench_count;            /* valid samples (<= BENCH_SAMPLE_CAP) */
    f64       bench_fps_accum;        /* rolling FPS over 1 s windows */
    i32       bench_fps_frames;
    f64       bench_fps_t0;
    /* The split behind bench_samples_ms. A "frame time" here is render +
     * present, and on a vsync'd Metal layer most of it is usually the block
     * inside -nextDrawable rather than GPU or CPU work. Reported apart so the
     * HUD says which of the three it is instead of implying the renderer is
     * slow. */
    f64       bench_wait_accum;       /* summed drawable+fence wait, this window */
    f64       bench_cpu_accum;        /* summed CPU-side render time, this window */
    i32       bench_split_n;          /* frames folded into the two sums */
    f64       bench_wait_ms;          /* last window's per-frame averages */
    f64       bench_cpu_ms;
    f64       bench_gpu_ms;           /* from the command buffer's own timestamps */
    u64       bench_gpu_ns_seen;      /* last snapshot of the monotonic GPU counters */
    u32       bench_gpu_cnt_seen;
    /* The rest of the chain — input latency, the PTY queue, loop stalls and
     * the per-frame grid work (ui/bench_metrics.h). Heap-allocated on the
     * first toggle beside bench_samples_ms, so a build with the HUD never
     * opened carries neither the ~15 KB of histograms nor any of the
     * recording. Every site that writes it is gated on bench_enabled. */
    struct BenchMetrics *bench_metrics;
    /* Process CPU/RAM sampling, refreshed at 1 Hz behind res_last_update so
     * the Mach calls don't fire per frame. */
    f32       res_cpu;
    f32       res_mem_mb;
    f64       res_last_update;

    /* Pane layout drag: Cmd+mousedown on a split pane arms a drag; on
     * mouseup over another pane edge the source pane's layout leaf is
     * moved to that side. Terminal+Session slots stay attached to their
     * pane ids; the split tree decides where each pane appears. */
    bool      pane_drag_pending;     /* Cmd+down landed on a pane, waiting threshold */
    bool      pane_drag_active;      /* threshold exceeded — drag is live */
    i32       pane_drag_tab_index;   /* tab the drag started in */
    i32       pane_drag_src_pane;    /* source pane index (0..N-1) */
    i32       pane_drag_hover_pane;  /* current pane under cursor; -1 = none */
    i32       pane_drag_drop_zone;   /* 1=left, 2=right, 3=top, 4=bottom */
    f32       pane_drag_start_x;     /* arming mouse position (fb px) */
    f32       pane_drag_start_y;
    /* Pane dragged up onto the tab bar → detach it into its own tab. Slot is
     * the tabs[] index the new tab is inserted before, or -1 to append. */
    bool      pane_drag_to_tabbar;
    /* How the drag's drop preview MOVES (ui/pane_drop_anim.h): the frame's
     * springs, the pane trading places with the dragged one, and the tab they
     * belong to. Stepped by the frame that draws it. */
    PaneDropAnim pane_drop_anim;
    i32          pane_drop_anim_tab;
    i32       pane_drag_tabbar_slot;
    /* A tab dragged out of ANOTHER window is hovering THIS window's tab strip.
     * This window hears none of that drag (the source holds mouse capture), so
     * main.c writes these from the source's motion events and the strip painter
     * draws the same seam caret the pane-detach drag above uses. Slot follows
     * the same contract: the tabs[] index the tab lands before, -1 = append.
     * Cleared by main.c on drop, cancel and when the pointer moves off. */
    bool      tab_xdrop_active;
    i32       tab_xdrop_slot;

    /* URL hover detection */
    TermURL   hover_url;
    bool      url_hover_active;
    i32       url_hover_col;      /* last cell checked — avoid re-scanning */
    i32       url_hover_row;
    /* The pane the hover belongs to. hover_url's rows/cols are grid
     * coordinates and every pane shares the same grid, so without this
     * the underline would be drawn by EVERY pane in a split at whatever
     * row/col the hovered one produced. Compared, never dereferenced. */
    Terminal *url_hover_term;
    /* What the detection read, so a frame can tell the text moved under a
     * still pointer and re-detect — see ui/url_hover.h. */
    UrlHoverSnap url_hover_snap;

    /* --- drag a selection past the edge of the grid -----------------------
     * A selection drag that leaves the terminal body scrolls it, one row at a
     * time, for as long as the button is held. It has to keep going with the
     * pointer STANDING STILL, so it cannot be driven from the mouse-move
     * handler — the loop steps it on a clock, and liu_continuous_visual()
     * carries it so the loop actually wakes for those steps.
     *
     * `dir` is -1 up / +1 down / 0 idle, `col` the column the pointer was
     * last over (the selection's free end keeps the user's column while the
     * rows move under it), `next` the wall time the next row is due. */
    i32       sel_scroll_dir;
    i32       sel_scroll_col;
    f64       sel_scroll_next;

    /* A text drag in the editor (code, text, Markdown Edit): the document it is
     * made in, which way the pointer has left the text (-1 above, 1 below, 0
     * inside) — a drag held past the edge scrolls on the frame clock, the
     * pointer standing still — the pointer's column, and when the next line is
     * due. Re-found among the live file browsers before every use. */
    FileBrowser *ed_drag_fb;
    i32       ed_drag_dir;
    f32       ed_drag_x;
    f64       ed_drag_next;

    /* Passphrase prompt dialog (shown when key auth needs passphrase) */
    bool      passphrase_dialog_active;
    Session  *passphrase_session;
    char      passphrase_key_path[512];
    char      passphrase_input[256];
    bool      ssh_use_mosh;      /* Use mosh instead of plain SSH */
    i32       ssh_dialog_proto;  /* 0=ssh, 1=telnet (host+port),
                                    2=serial (host=device, port=baud) */
    bool      ssh_forward_x11;   /* Request X11 forwarding */

    /* Drag & drop overlay */
    bool      drag_over_active;
    /* A file drag resting on a tab switches to it (ui/drag_spring.h). The
     * dwell is ticked from the frame loop, not from the drag events: the
     * pointer is standing still for the whole interval being measured. */
    DragSpringState drag_spring;

    /* SSH dialog recent history suggestions. Heap-allocated on first dialog
     * open (~4.7 KB) and freed in app_destroy. Capacity is SSH_HISTORY_CAP. */
    VaultHistoryEntry *ssh_history;     /* [SSH_HISTORY_CAP] when non-NULL */
    i32       ssh_history_count;
    i32       ssh_history_selected;  /* -1 = no selection */
    bool      ssh_recent_clear_armed; /* Recent's Clear was clicked once: the next click clears */
    /* Inline error banner shown inside the SSH dialog — set by submit
     * when preconditions fail (e.g. empty password). Cleared on char
     * input so the user sees feedback as they type. */
    char      ssh_dialog_error[160];
    /* The ~/.ssh/config alias the form was filled in from (a palette host
     * row), and the host text it was filled in WITH: the alias is what names
     * the host's IdentityFile and ProxyJump, and the form shows the resolved
     * address. Used at submit only while the Host field still reads the same. */
    char      ssh_dialog_alias[128];
    char      ssh_dialog_alias_host[256];
    f64       ssh_dialog_error_until;   /* platform_time_sec() expiry */

    /* Create Theme dialog (Cmd+K → Create Theme...). The user types a
     * theme name + a free-form visual description, picks a CLI agent
     * detected from PATH, and Liu spawns the agent in the background to
     * generate the JSON file under ~/.config/Liu/themes/. The same
     * fields drive both the modal UI and the long-running generation
     * task — the modal can be closed while the agent keeps running. */
    bool      create_theme_active;
    /* When true the dialog is "Create Note": one title field, no agent, no
     * generation. Submit creates <vault>/<slug>.md and opens it in the
     * Markdown editor, so this mode never leaves phase 0. */
    bool      create_theme_doc_mode;
    char      create_theme_name[64];
    char      create_theme_desc[512];
    i32       create_theme_name_len;
    i32       create_theme_desc_len;
    /* 0=name, 1=description, 2=agent picker (Tab cycles). */
    i32       create_theme_field;
    /* Index into the locally-detected agent list (cached on dialog open). */
    i32       create_theme_agent_idx;
    /* 0=input, 1=running, 2=success, 3=error. Driven by the spawn pipeline
     * in main.c; the modal renderer reads this to swap layouts. */
    i32       create_theme_phase;
    /* Short status / log line shown under the spinner while phase=1, and
     * carries the success/error message in phase 2/3. */
    char      create_theme_status[256];
    /* How far the generation has got, for the step list under the spinner:
     *   0 prompt sent · 1 agent thinking · 2 response streaming · 3 writing
     * the artifact. Derived from the same signals as the status line
     * (create_theme_update_live_status owns it) rather than tracked
     * separately, so the list and the line can never disagree about what
     * the agent is doing. Only meaningful while phase == 1. */
    i32       create_theme_stage;
    #define CREATE_THEME_STAGE_COUNT 4
    /* Inline error banner for input validation (empty name etc.). Same
     * pattern as ssh_dialog_error_until — auto-expires. */
    char      create_theme_error[160];
    f64       create_theme_error_until;
    /* Heap-allocated cache of CLI agents detected on dialog open.
     * NULL until first Create Theme open; freed in app_destroy. */
    struct AgentInfo *create_theme_agents;
    i32       create_theme_agent_count;
    /* Async generation state. */
    i32       create_theme_child_pid;     /* 0 when no spawn in flight  */
    PollGate  create_theme_poll;          /* this job's own 10 Hz clock (core/poll_gate.h) */
    i32       create_theme_stdout_fd;     /* read end of the pipe; -1 idle */
    /* Wall-clock at spawn time — drives the elapsed-time readout in the
     * running-phase modal. 0 when no spawn in flight. */
    f64       create_theme_spawn_time;
    /* Cumulative bytes read from the agent's stdout. The log buffer is
     * a rolling tail so its `_len` doesn't reflect total throughput;
     * this counter does, and feeds the "received" indicator. */
    i64       create_theme_total_bytes;
    /* Prompt size in bytes (recorded at spawn time). Used together with
     * total_bytes to surface a rough token estimate in the running modal:
     * a calibrated ~4-bytes-per-token heuristic, since the spawned agents
     * don't emit structured usage data. */
    i64       create_theme_prompt_bytes;
    /* Fingerprint the JSON object embedded in the submitted prompt. Some
     * agents echo the full prompt to stdout; the fingerprint lets the
     * finalizer distinguish the response object from an echoed example
     * before saving the theme. Zero means the prompt contained no object. */
    u64       create_theme_prompt_json_fp;
    char      create_theme_target_path[1024]; /* expected JSON destination */
    /* Captured agent stdout. Heap-allocated (64 KB) so a full theme JSON
     * (~3 KB) plus the agent's chatter fits with margin. Rolling tail:
     * once the cap is hit the contents shift down so the most-recent
     * bytes survive. The renderer only shows the last ~5 lines; the
     * parser scans the whole buffer for the JSON object after the agent
     * exits. */
    char     *create_theme_log;
    i32       create_theme_log_len;
    i32       create_theme_log_cap;

    /* Port forwarding dialog */
    bool      port_forward_dialog_active;
    u8        port_forward_mode;   /* 0=local, 1=socks5 */
    char      port_forward_local_port[8];
    char      port_forward_remote_host[256];
    char      port_forward_remote_port[8];
    i32       port_forward_field;  /* 0=local port, 1=remote host, 2=remote port */

    /* Keyboard-interactive / 2FA dialog. Heap-allocated on first dialog open
     * so the ~4 KB prompt/response pool only lives when the user is actually
     * seeing a 2FA challenge. The bool array stays inline (8 bytes, no point
     * allocating). */
    bool      kbi_dialog_active;
    Session  *kbi_session;
    char      kbi_name[256];
    char      kbi_instruction[512];
    char      kbi_peer[320];       /* "user@host" whose prompt this is */
    f64       kbi_shown_at;        /* when the dialog came up (Enter is not an answer before it is read) */
    i32       kbi_num_prompts;
    char    (*kbi_prompts)[256];     /* heap [KBI_MAX_PROMPTS][256] when non-NULL */
    bool      kbi_echo[KBI_MAX_PROMPTS];
    char    (*kbi_responses)[256];   /* heap [KBI_MAX_PROMPTS][256] when non-NULL */
    i32       kbi_field;

    /* File browser */
    FileBrowser  filebrowser;
    /* Which tab the live `filebrowser` above belongs to, or -1 when nobody
     * owns it yet. active_tab is written from a dozen places (close, move,
     * undo-close, drag reorder, restore-session…), so the sidebar is handed
     * over by comparing against THIS rather than by patching every one of
     * them — see app_sidebar_sync(). */
    i32          sidebar_owner_tab;

    /* Sidebar focus — set on any sidebar click, cleared on terminal click.
     * Gates the file-browser keyboard shortcuts (Cmd+C/X/V, Delete, F2, …)
     * so they don't steal keys from the terminal when the user isn't
     * actively working in the sidebar. */
    bool         sidebar_focused;

    /* File clipboard (last Cmd+C/X from a file-browser entry). */
    FileClipboard  file_clipboard;

    /* Was the open context menu's highlighted row put there by the arrow keys
     * (ui/ctx_menu.h: ctx_menu_pointer)? One flag for all four menus — only
     * one is ever open — and it needs no reset when a menu opens: a menu
     * opens with nothing highlighted, and the first pointer move inside it
     * clears the flag. */
    bool      ctx_menu_by_key;

    /* File-browser right-click context menu */
    bool      fb_ctx_menu_active;
    i32       fb_ctx_menu_entry;      /* entry index the menu was opened on */
    f32       fb_ctx_menu_x;
    f32       fb_ctx_menu_y;
    i32       fb_ctx_menu_selected;   /* highlighted row (pointer or arrows), -1 = none */

    /* The file browser's prompt: a name for a rename / a new file / a new
     * folder, or — FB_PROMPT_DELETE, no field — the question a permanent
     * delete asks first. Rows, rectangles and the rules for a name are
     * ui/fb_menu.h's; main.c's app_fb_prompt_* own the state.
     *
     * It stays open until what was asked HAPPENED: a name that cannot be
     * used says why under the field (`verdict`, checked on every edit), and a
     * refusal from the file system is quoted there too (`msg`). */
    bool      fb_prompt_active;
    u8        fb_prompt_mode;         /* FbPromptMode */
    i32       fb_prompt_index;        /* the entry renamed / deleted; -1 otherwise */
    char      fb_prompt_buf[256];
    i32       fb_prompt_len;
    u8        fb_prompt_verdict;      /* FbNameVerdict of fb_prompt_buf */
    char      fb_prompt_msg[128];     /* why the last Enter did not go through */
    char      fb_prompt_name[256];    /* the entry's own name (rename, delete) */
    char      fb_prompt_where[FB_MAX_PATH]; /* the folder it is, or will be, in */
    bool      fb_prompt_is_dir;
    i32       fb_prompt_count;        /* delete: how many items */
    u64       fb_prompt_generation;   /* the listing the indices belong to */

    /* Transient status line shown at the bottom of the sidebar: what the last
     * file operation did, by name, or why it did not (kind = FbStatusKind). */
    /* LIU_FBMENU_DEMO only: behave as a place with no Trash, so a delete asks. */
    bool      fb_no_trash;
    char      fb_status[128];
    u8        fb_status_kind;
    f64       fb_status_since;        /* wall-clock seconds */
    f64       fb_status_until;        /* absolute wall-clock seconds */

    /* Background paste/transfer task. The worker thread runs a snapshot of
     * the clipboard against a shimmed destination so a GB-scale transfer no
     * longer freezes the UI. Main thread joins the thread after `done`
     * flips true, refreshes the filebrowser, clears state. */
    bool      fb_task_active;           /* thread running or join pending */
    void     *fb_task_opaque;           /* FileOpsPasteTask * (main.c only) */
    /* One SFTP metadata operation (rename / delete / mkdir / create / move)
     * in flight off the UI thread. Each is a network round trip, so inside
     * the event handler it would wait behind the transfer worker's lock and
     * then the server's answer. */
    void     *fb_meta_opaque;           /* FbMetaTask * (main.c only) */

    /* Drag-and-drop within sidebar: the user pressed on an entry and is
     * dragging it. When the mouse is released on another folder row the
     * entry is moved there. `fb_drag_start_{x,y}` gate the transition
     * from click→drag so a tiny wiggle doesn't trigger a move. */
    bool      fb_drag_active;
    bool      fb_drag_open_on_release; /* modified clicks select without opening */
    bool      fb_os_drag_handed_off;   /* true once macOS took over the drag;
                                        * blocks re-entry and in-app drop. */
    i32       fb_drag_src_entry;
    f32       fb_drag_start_x;
    f32       fb_drag_start_y;
    f32       fb_drag_cur_x;
    f32       fb_drag_cur_y;

    /* Sidebar header drag — armed on mouse-down over the cwd breadcrumb
     * area (excluding the up/refresh/close buttons). Dropping on the tab
     * bar detaches the sidebar's filebrowser into a new FB tab. */
    bool      fb_header_drag_pending;
    bool      fb_header_drag_active;
    f32       fb_header_drag_start_x;
    f32       fb_header_drag_start_y;

    /* Passphrase cache — in-memory only, cleared on exit */
    PassphraseCacheEntry *passphrase_cache; /* heap-allocated [16] */
    i32       passphrase_cache_cap;
    i32 passphrase_cache_count;

    /* IME composition (preedit) state */
    char      ime_preedit[1028];   /* up to 256 code points of an X11 composition */
    i32       ime_cursor;
    bool      ime_active;

    /* Key manager overlay */
    bool      key_manager_active;
    KeyInfo  *key_list;            /* heap-allocated [32] */
    i32       key_list_cap;
    i32       key_list_count;
    i32       key_list_selected;
    i32       key_list_scroll;
    /* Key generation form */
    bool      keygen_form_active;
    i32       keygen_type;         /* 0=ed25519, 1=rsa-2048, 2=rsa-4096, 3=ecdsa-256, 4=ecdsa-384, 5=ecdsa-521 */
    char      keygen_filename[128];
    char      keygen_passphrase[256];
    char      keygen_passphrase2[256];
    char      keygen_comment[128];
    i32       keygen_field;        /* focused field: 0=filename, 1=passphrase, 2=confirm, 3=comment */
    char      keygen_status[256];  /* status/error message */
    /* Delete confirmation */
    bool      keygen_confirm_delete;
    i32       keygen_delete_idx;

    /* Close confirmation — armed instead of closing when an AI agent CLI is
     * running in the close target. tab/pane/group select what Enter closes:
     * pane >= 0 closes one split pane, group >= 0 closes a whole tab group,
     * otherwise the tab at close_confirm_tab. */
    bool      close_confirm_active;

    /* Paste prompt (ui/paste_confirm.h): a paste with line breaks into a program
     * that did not enable bracketed paste is held here until the user answers.
     * `paste_confirm_text` is a malloc'd copy of the clipboard, owned by this
     * struct and freed on every exit (answer, teardown). The target pane is
     * recorded by pointer and only ever COMPARED with the focused pane on
     * answer — a pane can close while the prompt is up. */
    bool         paste_confirm_active;
    char        *paste_confirm_text;
    u32          paste_confirm_flags;
    const Terminal *paste_confirm_term;
    const Session  *paste_confirm_sess;
    PasteSummary paste_confirm_sum;
    i32       close_confirm_tab;
    i32       close_confirm_pane;      /* -1 = whole tab */
    i32       close_confirm_group;     /* -1 = single tab */
    char      close_confirm_agent[32]; /* display name, e.g. "Claude Code" */

    /* Host key verification dialog */
    bool      hostkey_dialog_active;
    Session  *hostkey_session;
    char      hostkey_hostname[256];
    i32       hostkey_port;
    char      hostkey_old_fp[128];
    char      hostkey_new_fp[128];
    bool      hostkey_is_change;
    f64       hostkey_shown_at;    /* when the dialog came up: ui/hostkey_prompt.h arms on it */
    f64       hostkey_quiet_since; /* ...or the last key it swallowed: Enter arms on this */

    /* Known hosts viewer */
    bool      known_hosts_open;
    i32       known_hosts_selected;
    i32       known_hosts_scroll;
    char      known_hosts_filter[128];
    i32       known_hosts_filter_len;

    /* Quake mode */
    bool      quake_active;   /* true when quake window is visible/animating */

    /* Smart Vault overlays */
    bool      vault_unlock_dialog_active;
    char      vault_unlock_input[256];
    i32       vault_unlock_attempts;           /* consecutive wrong-password count */
    f64       vault_unlock_retry_after_ts;     /* monotonic seconds; 0 = allow */
    char      vault_unlock_error[128];         /* "", "Wrong password", "Locked for Ns" */
    /* If pending_action_on_unlock is non-zero, it's run once a successful
     * unlock lands (e.g. ACT_VAULT_BROWSER → open browser after unlock). */
    i32       vault_unlock_pending_action;
    bool      vault_unlock_is_init;            /* true when creating a fresh master pw */

    bool      vault_browser_active;
    i32       vault_browser_selected;
    i32       vault_browser_scroll;
    i32       vault_browser_kind_filter;       /* VaultSecretKind or -1 for all */
    char      vault_browser_filter[128];
    i32       vault_browser_filter_len;
    bool      vault_browser_reveal;            /* masked by default */
    f64       vault_browser_reveal_until_ts;   /* 0 = hidden, else absolute time */
    bool      vault_browser_editing;           /* true when edit form is open */
    i32       vault_browser_edit_field;        /* 0=label, 1=value, 2=env_name, ... */
    char      vault_browser_edit_label[128];
    char      vault_browser_edit_value[1024];
    char      vault_browser_edit_env_name[64];
    char      vault_browser_edit_host_id[64];
    char      vault_browser_reviewed_host_id[64];
    char      vault_browser_reviewed_env_name[64];
    VaultHost vault_browser_reviewed_recipient;
    i32       vault_browser_reviewed_kind;
    bool      vault_browser_binding_reviewed;
    bool      vault_browser_recipient_valid;
    bool      vault_browser_destination_visible;
    i32       vault_browser_review_page;
    i32       vault_browser_review_seen;
    i32       vault_browser_review_pages;
    i32       vault_browser_review_cols;
    i32       vault_browser_review_rows;
    i32       vault_browser_review_width;
    i32       vault_browser_review_height;
    f32       vault_browser_review_dpi;
    i32       vault_browser_edit_kind;         /* VaultSecretKind */
    bool      vault_browser_edit_is_new;       /* true = create, false = update */
    char      vault_browser_edit_secret_id[48];/* for update */
    char      vault_browser_status[128];
    f64       vault_browser_status_ts;
    /* The browser's row cache and the one decrypted plaintext it may be
     * showing, per window like every index into them (selected / kind_filter /
     * reveal). Shared, one window's filter would re-point another's rows, so
     * its Cmd+E / Cmd+C / Delete would act on an entry the user never
     * selected, and one reveal buffer against N reveal flags would print one
     * window's secret under another's label. `entries` is lazily allocated
     * (256 rows is ~130 KB, and most windows never open the browser); both are
     * released by app_vault_browser_close, which app_destroy already calls. */
    VaultSecret *vault_browser_entries;      /* VB_MAX_ENTRIES rows, or NULL */
    i32       vault_browser_entry_count;
    u8       *vault_browser_reveal_buf;      /* plaintext — scrubbed on release */
    usize     vault_browser_reveal_len;

    /* Renderer */
    Renderer  renderer;

    /* File-browser <-> terminal cwd auto-sync.
     *
     * When the active tab's local PTY changes its working directory (via
     * OSC 7, or fallback proc-pidinfo if the shell doesn't emit it), the
     * file browser navigates to the new path. The path last synced to is
     * tracked so only actual transitions act, and a user's manual fb
     * navigation is not reverted on every frame. Also tracks the active
     * tab index so switching tabs re-syncs once. */
    char      fb_sync_last_cwd[1024];
    i32       fb_sync_last_tab;
    f64       fb_sync_last_check_ts;
    /* Which terminal the last sync looked at + its OSC 7 seq at that time.
     * Pointer identity only (never dereferenced after free); a recycled
     * address simply triggers one extra immediate sync — safe direction. */
    const Terminal *fb_sync_last_term;
    u64       fb_sync_last_cwd_seq;
    /* The remote twin of the three fields above (ui.c's session poll): the
     * terminal and the folder report the explorer last followed over SFTP.
     * Compared, never dereferenced. */
    const void *fb_remote_sync_term;
    u64         fb_remote_sync_seq;

    /* Set by the last line of app_init(). app_destroy_window() also runs on
     * initialization failure, and only a fully initialized AppState may
     * write shared config and keybinding files. Otherwise a partially
     * initialized window could overwrite persisted settings with defaults or
     * zero values. */
    bool      initialized;

    /* Modal open/close transitions — one triplet per dialog, per window.
     * See ModalId above for why these cannot be function-local statics. */
    ModalAnim modals[MODAL_COUNT];

    /* Command-palette transition extras. Same trap as `modals`: shared
     * across windows these replayed one window's selection slide in
     * another, and `pal_was_rendering` drives palette_close() — a process
     * singleton — off a falling edge the wrong window would take. */
    i32       pal_last_selected;
    i32       pal_last_scroll;
    f32       pal_last_y_offset;
    Anim      pal_sel_anim;
    bool      pal_was_rendering;
    /* The palette's own motion (ui/palette_layout.h): how open it was drawn
     * last frame, and how open it was when it was last asked to open or to
     * close — a palette reopened on its way out turns round from there. */
    f32       pal_open_now, pal_open_from, pal_close_from;
    /* A list that ARRIVES (a sub-mode stepped into, a history scan landing)
     * brings its rows in one after the other from this moment; 0 = the list
     * on show was there all along. */
    f64       pal_enter_time;
    u8        pal_mode_was;         /* PaletteMode + 1 of the last frame drawn; 0 = none yet */
    bool      pal_scan_was;         /* ...and whether it was still waiting for its scan */
    /* The caret is solid just after a key and blinks once they stop. */
    f64       pal_key_time;
    u32       pal_query_sig;
    /* When a row was last taken (Enter or a click), and whether the close now
     * running is that: the taken row lights up as the panel goes. */
    f64       pal_pick_time;
    bool      pal_pick_closing;

    /* Sidebar open/close slide. These drive `sidebar_width`, which the
     * terminal layout, the hit test and the status bar all read. Shared as
     * statics, two windows would flip the visibility edge for each other
     * every frame and each would animate FROM the other's width, so a
     * window whose sidebar is shut would grow one it never opened. */
    bool      sidebar_anim_inited;
    bool      sidebar_anim_was_visible;
    f32       sidebar_anim_prev_width;
    f32       sidebar_anim_from;
    f32       sidebar_anim_to;
    Anim      sidebar_anim;

    /* Sectioned sidebar (ui/sidebar_layout.h). Window-level, deliberately
     * not per tab: the tab list IS the window's, and which sections are open
     * is a shape the user gave the window, not something a tab switch should
     * change under them. The explorer's own content stays per tab through
     * app_sidebar_sync. */
    bool      sidebar_sec_open[SB_SEC_COUNT];
    bool      sidebar_sec_inited;
    bool      sidebar_dock_prev;       /* config.sidebar_tabs last frame (edge → open) */
    f32       side_tabs_scroll;        /* px, the tab list's own scroll */
    /* Notes section: the vault's most recent .md files, rescanned on a TTL
     * because the list is painted every frame the sidebar is. */
    struct {
        char  name[SIDEBAR_NOTES_MAX_ROWS][96];
        char  path[SIDEBAR_NOTES_MAX_ROWS][1024];
        f64   mtime[SIDEBAR_NOTES_MAX_ROWS];
        i32   count;
        f64   scanned_at;
        char  root[1024];
        void *job;
    } side_notes;
    /* Agents section rows as painted this frame: row → (tab, pane), so the
     * hit test resolves a click without re-deriving the walk. */
    i32       side_agent_tab[SIDEBAR_AGENTS_MAX_ROWS];
    i32       side_agent_pane[SIDEBAR_AGENTS_MAX_ROWS];
    i32       side_agent_rows;
    i32       side_agent_total;        /* incl. the ones past the cap */
    /* The "Customize sidebar" popover (ui/sidebar_customize.h). Window-level
     * like the sections it edits. A press on a section row arms a reorder
     * drag that becomes real past SBC_DRAG_SLOP_PT; a release before that is
     * the row's click (its switch). */
    bool      sbc_open;
    i32       sbc_press_slot;          /* slot pressed, -1 none */
    bool      sbc_press_grip;          /* pressed on the grip: drag only, never a click */
    i32       sbc_drag_id;             /* SidebarSection being dragged, -1 none */
    f32       sbc_press_y;
    f32       sbc_drag_grab;           /* pointer y − the row's top at press */
    f32       sbc_drag_y;              /* pointer y now */
    bool      sbc_width_drag;          /* scrubbing the width slider */
    bool      sbc_dirty;               /* changed since open: save on close/release */
    /* Where the panel hangs, captured on the first frame after open and held
     * until it closes: it must not follow the bar it is resizing (a slider
     * that moves under the pointer that drags it never settles). */
    bool      sbc_anchor_set;
    f32       sbc_anchor_x;            /* the bar's right edge; < 0 = no bar, centre */
    f32       sbc_anchor_bottom;
} AppState;

/* Init/destroy
 *
 * Teardown is split because a window close is not a process exit.
 *   app_destroy_window()  — everything owned by ONE AppState: its tabs,
 *                           renderer, vault handle, per-app buffers, its
 *                           dev-server registry, its dialog state. Safe on a
 *                           half-built AppState (app_init memsets first).
 *   app_shutdown_process() — the module-level singletons that outlive any
 *                           single window (palette tables, shared host list,
 *                           model catalog/download, local-LLM worker, the
 *                           deferred tab-destroy thread). Call ONCE, after
 *                           the last window is gone.
 *   app_destroy()          — both, in order, for single-window callers and
 *                           the exit path.
 * Config + keybindings are written by whichever window is torn down LAST:
 * every AppState holds its own copy, so saving from a window that merely
 * closed would publish its stale copy over settings another window changed. */
bool app_init(AppState *app, i32 width, i32 height, f32 dpi_scale);
void app_destroy_window(AppState *app);
void app_shutdown_process(void);
void app_destroy(AppState *app);

/* Wire the shared terminal callbacks (response, bell, OSC 52, long-command,
 * command-finished) and their userdata to `app`. Exported because moving a
 * tab between windows has to re-own them: the AppState-valued userdata slots
 * are what a struct copy leaves aimed at the source window. */
void app_configure_terminal_callbacks(AppState *app, Terminal *term,
                                      Session *session);

/* Drop every pointer/index in `app` that aliases tab `index` — filebrowser
 * SFTP handle, the KBI/passphrase/host-key dialog sessions, outline_fb,
 * viewer_drag_source, the scroll-to-bottom rects, an in-flight pane drag, the
 * tab popup, the terminal context menu and the translate targets. Called by
 * app_close_tab(); also needed when a tab MOVES to another window, where the
 * tab survives but this AppState must stop naming it. */
void app_forget_tab_aliases(AppState *app, i32 index);

/* Re-resolve every live window's `config.theme` after theme_load_user_themes()
 * rebuilt g_user_themes[] — `config.theme` points INTO that array, so an
 * import or delete in one window otherwise leaves another window's pointer on
 * a different theme (or past the end). Also re-dirties the render caches,
 * because the ANSI LUT the terminals resolve through is process-global. */
void app_themes_reloaded(void);

/* Tab management */
i32  app_new_tab(AppState *app, Session *session, const char *title);
i32  app_new_filebrowser_tab(AppState *app, const char *path);

/* Open (or focus) an in-app browser tab for a loopback URL. Soft-caps at
 * BROWSER_MAX_TABS. site_path/site_name optional (Sites linkage).
 * `window` is required on macOS to attach WKWebView to SSHView. */
struct PlatformWindow;
i32  app_new_browser_tab(AppState *app, struct PlatformWindow *window,
                         const char *url,
                         const char *site_path, const char *site_name);

/* Open browser as a split pane of the active terminal tab (creates local
 * tab if needed). dir: SPLIT_H = left/right, SPLIT_V = top/bottom.
 * browser_first: true = browser on left/top. Returns tab index or -1. */
i32  app_open_browser_split(AppState *app, struct PlatformWindow *window,
                            SplitType dir, bool browser_first,
                            const char *url,
                            const char *site_path, const char *site_name);

/* Content rect of pane `pane` in framebuffer px (Y-down). False if invalid. */
bool app_tab_pane_rect(AppState *app, const Tab *tab, i32 pane,
                       f32 *x, f32 *y, f32 *w, f32 *h);

/* Scroll-to-bottom button hit test — index into app->scroll_btn[], or -1.
 * Only buttons published by the last frame are considered. */
i32  app_scroll_btn_hit(const AppState *app, f32 x, f32 y);

/* Image-file badge hit test — index into app->img_badge[], or -1 — and the
 * open that follows it. Both read the per-frame registry the badge pass
 * publishes; see the AppState field. */
i32  app_img_badge_hit(const AppState *app, f32 x, f32 y);
bool app_img_badge_open(AppState *app, i32 index);

/* Prompt-recall badge hit test — index into app->prompt_badge[], or -1 — and
 * the expand/collapse the click performs. Same per-frame registry arrangement
 * as the image badge above. The hit test is asked BEFORE mouse reporting is
 * forwarded, for the reason the image badge documents: four of the five
 * alt-screen agent CLIs grab the mouse, so a badge that let the click through
 * would be dead in exactly the sessions it exists for. */
i32  app_prompt_badge_hit(const AppState *app, f32 x, f32 y);
bool app_prompt_badge_toggle(AppState *app, i32 index);

/* Attachment-tile hit test — index into app->prompt_thumb[], or -1 — and the
 * lightbox open that goes with it. The path is resolved from the pane's live
 * PromptRecall at click time rather than cached in the registry, the same rule
 * app_img_badge_open follows: a pane can close, or its prompt can be replaced,
 * between the frame that published the rect and the press. */
i32  app_prompt_thumb_hit(const AppState *app, f32 x, f32 y);
bool app_prompt_thumb_open(AppState *app, i32 index);

/* Expand or collapse a pane's prompt card, carrying the grow animation with it.
 * The ONLY way `PaneAgent.prompt_expanded` may change — set directly, the eased
 * geometry is computed against a stale animation and the card snaps. */
/* Which agent owns this pane, consulting the OS when the 0.75 s tool cache
 * does not know yet. For one-shot user actions only — see the note on the
 * implementation; a per-frame caller must read PaneAgent.tool directly. */
i32 app_pane_agent_adopt_live(AppState *app, Terminal *t, Session *s);

void app_prompt_set_expanded(PaneAgent *pa, bool expanded);

/* Re-open or re-close every pane's recall card to match config.prompt_recall.
 *
 * The mode picks a card's DEFAULT state and a click overrides it for the rest
 * of that prompt, so nothing in the per-pane state reacts to the mode changing
 * underneath it — switch to Always and the panel would stay shut until the
 * next prompt was sent, which reads as a control that did nothing. Both places
 * that can change the mode (the ⌘K commands and the Settings segmented
 * control) call this, so the two surfaces cannot drift into different
 * behaviour. Off needs no work: render_prompt_recall returns on the mode. */
void app_prompt_recall_apply_mode(AppState *app);

/* Throw away the multi-line prompt being composed for `pa`, if it owns the
 * window's compose buffer. Called on the turn's rising edge: the agent
 * starting means whatever was half-typed is over, and keeping it would let it
 * be appended to by the NEXT prompt ("aaa" + Ctrl+C + "bbb" would show as one
 * badge reading "aaa bbb"). Passing NULL drops it unconditionally. `pa` is
 * only COMPARED: a pane can close mid-compose. */
void app_prompt_compose_drop(AppState *app, const PaneAgent *pa);
/* Drop a pane's prompt and every animation attached to it. */
void app_prompt_recall_forget(PaneAgent *pa);

/* This pane's agent state, or NULL when the terminal is not a live pane of any
 * tab. The per-poll cache tab_update_pane_agents() maintains is the only place
 * agent state is written; every reader — including the prompt capture on the
 * keypress path — goes through a lookup like this rather than probing. */
PaneAgent *app_pane_agent_for_terminal(AppState *app, const Terminal *term);

/* LIU_PROMPT_PROBE=1 — log what the prompt recall captured and where its rect
 * landed. Neither is observable from outside the app: the chip only exists for
 * a pane running an agent that has been prompted. */
bool prompt_recall_probe_on(void);

/* Drop a terminal's scroll-button fade slot. Must be called before a Terminal
 * is destroyed — the pool keys on the pointer and never revisits a pane that
 * stopped rendering. */
void app_scroll_btn_forget(const Terminal *term);

/* Push every window-background setting (translucency, opacity, backdrop blur
 * and the background image) from config onto the window and the renderer.
 *
 * One entry point on purpose: these four interact (blur needs a non-opaque
 * window; the image sits under text that is itself drawn with `opacity`), so
 * they are never applied one at a time from separate call sites. Safe to call
 * often: the image only reloads when its path changed. */
void app_apply_window_background(AppState *app, struct PlatformWindow *window);

/* Terminal context menu rows.
 *
 * The row list is contextual (Copy appears only when the pane had a selection
 * when the menu opened), so the renderer and the click handler must not each
 * spell it out. Both call app_term_menu_rows() and index the SAME array; a
 * hardcoded row number in either place would make "Split Left" fire when the
 * user clicked "Rename Tab". */
typedef enum {
    TERM_MENU_COPY = 0,
    TERM_MENU_OPEN_LINK,     /* only when the right-click landed on a link */
    TERM_MENU_COPY_LINK,
    TERM_MENU_RENAME_TAB,
    TERM_MENU_SPLIT_RIGHT,
    TERM_MENU_SPLIT_LEFT,
    TERM_MENU_SPLIT_DOWN,
    TERM_MENU_SPLIT_UP,
} TermMenuAction;

#define TERM_MENU_MAX_ROWS 8

/* Fill `out` with the rows to show, in order, and `sep_after[i]` with whether a
 * separator follows row i. Returns the row count. */
i32  app_term_menu_rows(const AppState *app, TermMenuAction *out,
                        bool *sep_after, i32 cap);

/* Label for a row, and the small leading glyph cell it draws (see the
 * renderer). Kept beside the row list so the two can't drift. */
const char *app_term_menu_label(TermMenuAction a);

/* Full laid-out menu: box in framebuffer px, the row list, and the top Y of
 * every row (separators already accounted for). Any out-param may be NULL.
 * The box is clamped to the window here, so the caller never repeats that.
 * Returns the row count. */
typedef struct {
    f32            x, y, w, h;
    i32            count;
    TermMenuAction acts[TERM_MENU_MAX_ROWS];
    bool           sep_after[TERM_MENU_MAX_ROWS];
    f32            row_y[TERM_MENU_MAX_ROWS];
    f32            row_h;
} TermMenuLayout;

i32  app_term_menu_layout(const AppState *app, TermMenuLayout *out);

/* The file browser's context menu, laid out for what it was opened on — the
 * one answer the renderer, the hover tracker, the arrow keys and the click
 * handler share (ui/fb_menu.h holds the rules). */
void app_fb_menu_target(const AppState *app, FbMenuTarget *out);
/* Can a LOCAL delete in this window be taken back (is there a Trash)? */
bool app_fb_trash_available(const AppState *app);
void app_fb_menu_layout(const AppState *app, FbMenuLayout *out);
/* The explorer's status line, as the loop's three gates need it. */
static inline bool app_fb_status_fading(const AppState *a, f64 now) {
    return a && a->fb_status[0] &&
           fb_status_fading(now - a->fb_status_since, a->fb_status_until - a->fb_status_since);
}
static inline f64 app_fb_status_wake_in(const AppState *a, f64 now) {
    if (!a || !a->fb_status[0]) return 0.0;
    return fb_status_wake_in(now - a->fb_status_since, a->fb_status_until - a->fb_status_since);
}

/* Row index under a point, or -1 when the point misses the rows (including the
 * separator gaps). Shared by the click handler and the hover highlight. */
i32  app_term_menu_row_at(const AppState *app, f32 x, f32 y);

/* Bracketed paste into a PTY (shared for clipboard + browser attach).
 * Browser context uses TARGET_ONLY so it cannot reach another broadcast PTY. */
#define APP_PASTE_F_SKIP_TRANSLATE_SHADOW 1u
#define APP_PASTE_F_TARGET_ONLY 2u
/* The caller has already committed to running this text (a snippet the user
 * picked, an agent command, a path dropped on a pane), or the user has just
 * said yes in the paste prompt: do not ask. */
#define APP_PASTE_F_NO_CONFIRM 4u
void app_send_bracketed_paste(AppState *app, Terminal *t, Session *s,
                              const char *text);
void app_send_bracketed_paste_ex(AppState *app, Terminal *t, Session *s,
                                 const char *text, u32 flags);

/* Inject a browser element pick into the focused/available agent chat.
 * force_attach: ignore busy heuristic. Returns true if pasted to a PTY. */
bool app_browser_inject_pick(AppState *app, const BrowserPick *pick,
                             const char *project_root, bool force_attach);

/* Inject a browser connection error ("Ask Agent" on the error page) into the
 * focused/available agent chat. Returns true if pasted to a PTY. */
bool app_browser_inject_error(AppState *app, const char *url,
                              const char *reason, i32 code,
                              const char *project_root);

/* Push a navigated URL to the front of the MRU browser recents (dedup + cap),
 * updating browser_last_url too. */
void app_browser_push_recent(AppState *app, const char *url);

/* Serialize browser recents as a JSON array of URL strings for the native
 * start page (e.g. ["http://localhost:5173/"]). Returns bytes written. */
usize app_browser_recents_json(AppState *app, char *out, usize cap);

/* Push the element-picker inspector theme (highlight box + tooltip colors)
 * into a browser tab's page, derived from the ACTIVE terminal theme. Call
 * before arming pick mode; cheap, safe to repeat. */
/* Bind the shared single-line editing state (AppState.te_*) to a chrome
 * field, then feed keys/characters to ui/textedit.h. Focus is implicit:
 * whichever field binds owns the caret, selection and undo until another
 * one does — exactly one field has the keyboard at a time. Call
 * app_edit_done() with the field's new length after any edit.
 *
 * app_edit_ex() takes TEXTEDIT_* flags (SECRET for masked fields, DIGITS for
 * numeric ones). Secret fields should pass a NULL undo history so the
 * plaintext is not also held in a snapshot ring. */
/* Draw a chrome text field's value with the shared editor's caret, selection
 * band and horizontal scroll. Any field routed through app_edit()/textedit_key
 * MUST draw through here: the caret can stand anywhere in the string, so a
 * field that paints it at the end of the string puts it in the wrong place.
 * `mask` renders bullets for secret fields. Returns the caret's x. */
/* One row of a context menu, for the ONE painter every menu uses
 * (ctx_menu_draw_rows, ui.c; placement is ui/ctx_menu.h). */
typedef struct {
    IconKind    icon;       /* ICON_NONE: none — the label still starts past the rail */
    const char *label;
    const char *keys;       /* the shortcut, in keybind_format's spelling; "" = none */
    bool        enabled;    /* false: dim, takes no highlight */
    bool        danger;     /* the row that destroys something: the danger colour */
} CtxMenuRow;
/* How wide a menu with these rows wants to be; its panel; its rows (the 8x16
 * pt UI scale, flushed). */
f32  ctx_menu_measure(const CtxMenuRow *rows, i32 count, f32 dpi);
void ctx_menu_draw_panel(Renderer *r, const Theme *t, f32 x, f32 y, f32 w, f32 h, f32 dpi);
void ctx_menu_draw_rows(Renderer *r, const Theme *t, const CtxMenuBox *b,
                        const CtxMenuRow *rows, i32 selected, f32 dpi);
/* The shared chrome of a floating panel, the hover wash of a control, and a
 * string fitted into `cols` display columns with an ellipsis. */
void draw_panel_chrome(Renderer *r, f32 x, f32 y, f32 w, f32 h,
                       Color body, Color border, f32 radius, f32 dpi,
                       f32 shadow_size, f32 shadow_alpha);
void ui_hover_wash(AppState *app, Renderer *r, f32 x, f32 y, f32 w, f32 h,
                   f32 radius, Color tint, f32 peak);
const char *ui_fit_cols(const char *s, i32 cols, char *buf, usize cap);
/* The UI's text: one step per cell, two for a wide character. */
void draw_text_ex(Renderer *r, const char *text, f32 x, f32 y, Color fg, i32 max_chars, f32 step);
f32 ui_draw_edit_field(Renderer *r, AppState *app, const char *buf, i32 len,
                       f32 x, f32 y, f32 cw, f32 ch, i32 max_cells,
                       Color fg, Color caret_c, Color sel_c,
                       bool mask, bool show_caret);
/* Shared caret blink phase (macOS cadence, strong floor). */
f32 ui_caret_alpha(void);
/* The text field that has the keyboard, at its caret (x, y: the caret's top;
 * cw, ch: the field's cell): an input method composes here and its candidate
 * list hangs from here, not from the pane under the field. Draws the
 * composition (none in a `mask` field). `owner` names the field (its buffer)
 * for ui/ime_target.h. ui_draw_edit_field calls it; a field that draws its own
 * caret calls it where it draws the caret. */
void ui_ime_field_caret(Renderer *r, AppState *app, const void *owner,
                        f32 x, f32 y, f32 cw, f32 ch, bool mask);

/* Text-area registry: which parts of the chrome are editable text.
 *
 * The mouse-move handler needs this to place the I-beam over editable text
 * only, not across a whole panel that is mostly buttons and rows, in line
 * with the terminal's own rule (arrow by default, I-beam only during a
 * selection drag). Fields register themselves as they are laid out; anything
 * unregistered keeps the arrow.
 *
 * Reset once per frame at the top of app_render, so a field that stops being
 * drawn stops claiming its rect. The mouse-move handler reads the PREVIOUS
 * frame's set: the same one-frame lag the settings hover registry runs on,
 * invisible at any interactive pointer speed. */
void ui_text_area_reset(void);
void ui_text_area_push(f32 x, f32 y, f32 w, f32 h);
bool ui_text_area_hit(f32 x, f32 y);

TextEdit app_edit(AppState *app, char *buf, i32 *len, i32 cap);
TextEdit app_edit_ex(AppState *app, char *buf, i32 *len, i32 cap, u32 flags);
void     app_edit_done(AppState *app, i32 len);

void app_browser_send_pick_theme(AppState *app, BrowserTab *b);
/* Push the active terminal theme into the browser's own documents (start page,
 * error page, WebView backdrop). Idempotent; records BrowserTab.themed_as so
 * the render loop can re-push after a theme switch. */
void app_browser_send_page_theme(AppState *app, BrowserTab *b);

/* Open/focus a browser tab and arm Select Element (◎). Uses running Site
 * port or browser_last_url when no browser is open. Returns browser tab
 * index or -1. */
i32  app_browser_begin_select_element(AppState *app,
                                      struct PlatformWindow *window);

/* Re-inject the most recent pick (any browser tab's last_pick, prefer
 * active). force_attach=true. Returns true if pasted or copied. */
bool app_browser_attach_last_pick(AppState *app);
/* Refresh `tab->title` from the FB tab's current cwd. Honors HOME (`~`)
 * and root (`/`). Does nothing if the tab has a custom_title. No-op for
 * non-FB tabs or tabs without an FB instance. */
void app_refresh_fb_tab_title(Tab *tab);
/* Foreground AI-agent probe for the close confirm. Returns the running
 * agent's ChatTool as i32 (0 = none) and optionally its display name
 * ("Claude Code", "Codex", …). The tab variant scans every split pane. */
i32  app_tab_running_agent(AppState *app, i32 tab_index, const char **display);
i32  app_pane_running_agent(AppState *app, i32 tab_index, i32 pane, const char **display);

/* Split-pane accessors. Exposed for the tab popup, which mirrors a tab's
 * split geometry at thumbnail scale — it must lay panes out with the SAME
 * tree walk the real renderer uses or the preview would misrepresent the
 * layout it is previewing. `app_tab_pane_layout` writes one rect per pane
 * inside the (x, y, w, h) box; unused pane slots are left zeroed. */
i32       app_tab_pane_count(const Tab *tab);
Terminal *app_tab_pane_terminal(const Tab *tab, i32 pane);
void      app_tab_pane_layout(const Tab *tab, f32 x, f32 y, f32 w, f32 h,
                              f32 dpi, PaneRect out[MAX_SPLIT_PANES]);
void app_close_tab(AppState *app, i32 index);
void app_switch_tab(AppState *app, i32 index);
/* Move the live sidebar/file-browser state into tab `index`'s stow, leaving
 * app->filebrowser zeroed. Safe to call with a stale or out-of-range index. */
void app_sidebar_stow(AppState *app, i32 index);
/* The other direction — hand tab `index`'s stowed state back to app->. */
void app_sidebar_restore(AppState *app, i32 index);
/* Hand the sidebar to whichever tab is active now, if it moved. Cheap no-op
 * when ownership already matches; call it before anything reads
 * app->filebrowser in a frame. */
void app_sidebar_sync(AppState *app);
/* Call `fn` on every FileBrowser this window owns, exactly once: the live
 * sidebar, each tab's own, and each tab's stowed sidebar (zeroed when the tab
 * is the owner). A zeroed browser is passed too — `fn` must treat it as empty. */
void app_for_each_browser_in(AppState *a, void (*fn)(FileBrowser *, void *), void *ctx);
/* Accept an asynchronous restore request; sleeping stays true until adoption. */
bool app_wake_tab(AppState *app, i32 index);
void app_sleep_inactive_tabs(AppState *app, f64 now_sec);
/* Tabs currently asleep; reported by the frame probe as `slept=`. */
i32 app_sleeping_tab_count(const AppState *app);
/* Banners an in-band notification asked for, and the last one's "title|body"
 * (process-wide; for the driver's state). */
u32 app_banner_count(void);
const char *app_banner_last(void);

/* Does one of Liu's OWN password fields have the keyboard in this window (the
 * SSH form's password, a key's passphrase, the vault's master password)?
 * Those are drawn by Liu, so nothing protects them unless Liu does
 * (ui/secure_input.h). */
bool app_password_field_focused(const AppState *app);

/* Is anything at work in one of this window's panes — a job that owns a local
 * pane's terminal, or a remote link that is up? Two cheap calls per local
 * pane; asked about once a second by the loop to decide whether the app may
 * be napped (core/activity_hold.h). A sleeping tab is not at work. */
bool app_any_pane_at_work(AppState *app);
bool app_sleep_tab(AppState *app, i32 index);
#ifdef LIU_SLEEP_SNAPSHOT_PROBE
bool app_sleep_snapshot_bench(AppState *app);
#endif
/* Returns why a tab cannot be slept; `SLEEP_OK` means sleeping is allowed.
 * Explicit requests bypass only the idle-only rules documented in
 * `ui/sleep_policy.h`. */
SleepBlock app_tab_sleep_block(AppState *app, i32 index, bool explicit_request);
void app_show_toast(AppState *app, const char *message);
/* The same with the kind named, for a caller that knows better than the
 * words do (notification_kind_from_text reads those). */
void app_show_toast_kind(AppState *app, NotificationKind kind, const char *message);
/* A notice ABOUT something (`about`, an address that names it, e.g. a Site):
 * one still up about the same thing is replaced in its place rather than
 * stacked under (ui/toast_stack.h). */
void app_show_toast_about(AppState *app, const void *about, NotificationKind kind,
                          const char *message);
/* Whether any floating notice is up or on its way out (the loop's gates). */
bool app_toast_active(const AppState *app, f64 now);
/* A click at (x, y) in framebuffer pixels: dismisses the notice under it and
 * returns true (the click is spent), false when none is there. */
bool app_toast_click(AppState *app, f32 x, f32 y);
/* Write `text` to the system clipboard and SAY what happened. A refusal
 * (over the size ceiling, not UTF-8, the system said no) always toasts the
 * reason — the pasteboard still holds what it held before, and a silent
 * failure is a paste of the WRONG text later. `success_toast` is shown on
 * success when non-NULL; Cmd+C passes NULL, since a copy key is silent in
 * every app. Returns true when the clipboard now holds `text`. */
bool app_clipboard_copy(AppState *app, const char *text, const char *success_toast);
/* How many app_clipboard_copy calls have succeeded — for LIU_SEL_DEMO, which
 * asserts that one selection gesture writes the clipboard exactly once. */
u32  app_clipboard_copy_count(void);

/* Render benchmark HUD: toggled from the command palette; app_bench_record is
 * called once per painted frame from the main loop with that frame's ms. */
void app_bench_toggle(AppState *app);
void app_bench_record(AppState *app, f64 frame_ms, f64 wait_ms);
void app_show_crash_banner(AppState *app, const char *reason, const char *path);
void render_crash_banner(AppState *app);
void app_show_info_banner(AppState *app, const char *line1, const char *line2);
void render_info_banner(AppState *app);
Tab *app_active_tab(AppState *app);
/* Drop a pane's agent state, releasing what its prompt owns (the attachment
 * block, see prompt_capture.h). The way to EMPTY a PaneAgent slot that was not
 * moved from; a slot that was moved from is memset by its mover. */
void pane_agent_reset(PaneAgent *pa);
/* The same for every pane slot of a tab — on the tab's way out. */
void tab_pane_agents_reset(Tab *tab);

/* If a full-window Graph View owns the active tab, return the folder it is
 * graphing (the notes Vault root, since "Graph View" rescopes there); NULL when
 * no live graph is up. Lets sidebar-open paths mirror the graphed Vault instead
 * of dropping into the terminal's cwd. */
const char *app_active_graph_root(AppState *app);

/* Undo close: restore the most recently closed tab */
bool app_undo_close_tab(AppState *app);

/* Layout recalculation */
void app_update_layout(AppState *app, i32 win_w, i32 win_h, i32 fb_w, i32 fb_h);

/* Apply config changes (font, theme, etc.) */
void app_apply_config(AppState *app);

/* Render one frame */
/* Take ownership of this window's async decode results, without
 * needing a paintable window. Cheap when idle (an atomic load per producer);
 * returns true when something was installed, i.e. when a visible window would
 * now draw something new. The main loop calls this for every window every
 * iteration — see the comment on the implementation for why it cannot stay
 * inside app_render. Shared prompt/history results are pumped separately. */
void app_async_frame_begin(void);
bool app_pump_async_completions(AppState *app);

void app_render(AppState *app);
bool app_render_prepare_ready(const AppState *app);

/* May this pane paint the Cmd+hover link underline?
 *
 * hover_url carries GRID coordinates and render_terminal_pane runs once per
 * pane, so the answer is "only the pane the hit came from" — an ungated draw
 * underlined the same rows and columns in every pane of a split, over text
 * that is not a link in any of them but one. The terminal is COMPARED, never
 * dereferenced: a pane can close under a held Cmd. */
static inline bool ui_url_hover_owns(const AppState *app, const Terminal *term) {
    return app && term && app->url_hover_active &&
           app->hover_url.url[0] && app->url_hover_term == term;
}

/* Does the DOCKED document — the code editor or Markdown reader beside the
 * terminal — have the keyboard? It does from the moment it is opened or
 * clicked until a click lands anywhere else (FileBrowser.kbd_away). Every
 * place that sends a key, a character or Find to that document asks this, and
 * so does the terminal's cursor: exactly one of the two shows a caret. */
static inline bool app_docked_doc_has_keyboard(const AppState *app) {
    return app && app->filebrowser.view_mode != FVIEW_NONE && !app->filebrowser.kbd_away;
}

/* Render a terminal grid (primary or scrollback) into a rect at (origin_x,
 * origin_y), using `cache` for per-row dirty tracking. Exposed so the Sites
 * panel can render each site's headless log terminal. */
void render_terminal_pane(AppState *app, Terminal *term, TermRenderCache *cache,
                          f32 origin_x, f32 origin_y, bool focused);
/* The same, for a pane of the active tab: `bar_right` is the RIGHT edge of the
 * pane's slice of the body, where its own scrollbar goes (and what the scroll
 * button and the prompt card keep clear of). <= 0 draws no bar — the plain
 * form above is that, for surfaces that are not tab panes. */
void render_terminal_pane_ex(AppState *app, Terminal *term, TermRenderCache *cache,
                             f32 origin_x, f32 origin_y, bool focused, f32 bar_right);

/* Process I/O for all sessions. Returns true if any data was received. */
bool app_poll_sessions(AppState *app);
/* Fold one agent-hook signal (notify/agent_signal.h) into the pane it belongs
 * to. False when that pane is not in this window — the ring is process-wide,
 * so the main loop offers each signal to every window until one takes it. */
bool app_apply_agent_signal(AppState *app, const AgentSignal *sig, f64 now);
/* Re-derive the "N working" counts of tabs whose working deadline has lapsed
 * without a session poll to notice. Once per loop turn; true when one changed. */
bool app_agent_states_settle(AppState *app, f64 now);

/* Reset the process-wide PTY read budget. MUST be called once per main-loop
 * iteration, before the first app_poll_sessions() of that iteration — the poll
 * runs once per WINDOW, so a reset inside it makes the shared total
 * per-window. core/poll_budget.h has the policy. */
void app_poll_frame_begin(void);
/* Drain the focused echo before rotating the order of whole windows. Returns
 * the bytes read (0 when nothing was, or it already ran this turn). */
i64 app_poll_focused_session(AppState *app);

/* Read-only view of that ledger, for the frame probe and the bench HUD. */
const PollLedger *app_poll_ledger(void);
PollLedger       *app_poll_ledger_mut(void);

/* True while the voice card is on screen and animating itself (a live
 * session, or LIU_VOICE_DEMO). The main loop's wake and repaint gates both
 * need this — a clock-driven visual in only one of them freezes as soon as
 * the pointer stops moving. */
bool app_voice_hud_animating(void);
/* Set the synthetic voice HUD preview override. The preview shows the card
 * without opening the microphone; `false` forces the preview off regardless of
 * `LIU_VOICE_DEMO`. */
void app_voice_hud_demo_set(bool on);

/* Tab reorder */
void app_move_tab(AppState *app, i32 from, i32 to);

/* Split pane */
void app_split_tab(AppState *app, SplitType dir);
void app_resize_tab_panes(AppState *app, Tab *tab);
/* Reconcile every tab whose grid was left behind by a deferred layout pass.
 * Cheap no-op when nothing is pending. */
void app_flush_pending_resizes(AppState *app);
/* Width the diff dock currently reserves on the right, 0 when closed. The
 * terminal grid subtracts it, so opening the dock reflows rather than
 * overlaying. */
f32  app_git_diff_dock_w(const AppState *app);
/* Width the docked file viewer reserves on the right (0 when closed).
 * The one clamp the panel, the hit test and the terminal grid share. */
f32  app_file_viewer_w(const AppState *app);
/* True while a markdown code block's "Copied" confirmation is still fading, on
 * either markdown surface. In liu_continuous_visual()'s list. */
bool app_md_copy_flash_active(const AppState *app, f64 now);
/* A dialog, panel or menu is in front of the panes and has the pointer:
 * nothing under it is hovered or told of it (ui.c, the one list). */
bool app_dialog_has_pointer(const AppState *a);
/* True while the glyph atlas has been relaid out since this window's row
 * caches were built — a grow or a shelf reclaim re-normalises every published
 * UV, and the caches hold those UVs baked into GPU instances. The loop turns
 * it into a repaint the way it does the caret and the toast, because a
 * relayout on the last completion of a batch otherwise owes nobody a frame. */
bool app_atlas_layout_stale(const AppState *app);

/* THE ONE GATE for "is this visible row drawn in BiDi visual order", and the two
 * coordinate maps that go with it.
 *
 * Returns false when the row is drawn in logical order, in which case both maps
 * are the identity and the caller can use its column unchanged. Either map may be
 * NULL; each needs room for BIDI_MAX_COLS (1024) entries.
 *
 * The render passes and the input side both ask it: the hit test turns a pixel
 * into a VISUAL column, while selection_start/extend/update address LOGICAL
 * columns. On three Hebrew letters the map is 2,1,0: a click on the left-hand cell
 * is logical 2, and read as 0 it would put the selection on the letter at the
 * other end of the word, so Cmd+C would return text that was never highlighted.
 *
 * `row` is a VISIBLE row (0..rows-1), resolved through terminal_visible_row the
 * way every other pass over the screen does, which also means it invalidates that
 * function's shared scratch buffer, so do not call it while holding a row pointer.
 *
 * NOT for TUI mouse reporting: a program that grabbed the mouse is drawing its own
 * screen and wants the PHYSICAL column, whatever direction the text ran. */
bool app_term_row_bidi_map(Terminal *term, i32 row,
                           i32 *logical_to_visual, i32 *visual_to_logical);
/* The column a pixel-derived VISUAL column names logically, for one visible row.
 * Returns `vis_col` unchanged for a row that is not reordered, and clamps to the
 * grid. The convenience form of app_term_row_bidi_map for the input paths. */
i32  app_term_logical_col(Terminal *term, i32 row, i32 vis_col);

/* Sum of everything docked against the window's right edge (git dock + file
 * viewer). What the terminal box, the grid, the hit test and the scrollbar
 * all measure against. */
f32  app_right_reserve(const AppState *app);
/* Same as app_split_tab but lets the caller place the *new* pane on the
 * leading edge (left for SPLIT_H, top for SPLIT_V) when `new_first` is true.
 * Used by the right-click context menu's Split Left / Split Up entries. */
void app_split_tab_dir(AppState *app, SplitType dir, bool new_first);

/* Tab-bar drag → split drop.
 *
 * `src_idx` is the dragged tab, `dst_idx` the tab that hosts the split (the
 * tab that was open before the drag started — see app_split_drop_dst), and
 * `zone` is 1=left, 2=right, 3=top, 4=bottom. The dragged tab's terminal +
 * session are *moved* into the destination as a new pane on that edge and
 * the (now empty) source tab entry is closed. Works whether the destination
 * is single-pane or already split — an already-split destination gets the
 * new pane grafted at the root so the drop edge still means the whole tab.
 * Returns false (with a toast) when the drop can't be honoured. */
bool app_split_tab_from_drag(AppState *app, i32 src_idx, i32 dst_idx, i32 zone);

/* Resolve which tab should host a split-drop of `src_idx`: the tab that was
 * active just before the drag began (app->tab_drag_prev_active) when that is
 * still a usable terminal tab, otherwise the nearest usable neighbour.
 * Returns -1 when no other tab can host a split. */
i32  app_split_drop_dst(AppState *app, i32 src_idx);

/* Terminal content area in framebuffer pixels — the canonical rect that both
 * the split-drop hit test and its overlay must agree on. */
void app_terminal_area_rect(AppState *app, f32 *x, f32 *y, f32 *w, f32 *h);

/* Split-drop zone under (px, py): 1=left, 2=right, 3=top, 4=bottom, 0 when
 * inside the terminal area but in the neutral centre, -1 when outside. */
i32  app_split_drop_zone_at(AppState *app, f32 px, f32 py);

/* Geometry the dropped pane will occupy for `zone` (half the terminal area on
 * that edge). Also yields the rect the destination's existing content gets
 * pushed into. Both may be NULL. */
void app_split_drop_preview(AppState *app, i32 zone,
                            f32 *nx, f32 *ny, f32 *nw, f32 *nh,
                            f32 *ex, f32 *ey, f32 *ew, f32 *eh);

/* Collapse a split tab back to a single pane by killing one side.
 * `pane` is 0 (left/top) or 1 (right/bottom); the surviving pane is
 * resized to the full tab grid and the dead pane's Terminal/Session
 * are handed to the background trash queue. No-op when not split. */
/* How many panes this tab has, with the same clamping every caller inside
 * ui.c gets. Exported so ui/app_soak.c can ask the question rather than
 * re-deriving it from split_pane_count and drifting from the clamp. */
i32  tab_split_pane_count(const Tab *tab);

void app_close_split_pane(AppState *app, i32 tab_index, i32 pane);

/* Pull `pane` out of a split tab and give it its own tab — the inverse of
 * app_split_tab_from_drag. The pane's Terminal + Session are *moved* (never
 * destroyed), the source tab collapses around the hole, and the new tab is
 * inserted before `insert_at` (-1 appends) and made active. Returns the new
 * tab's index, or -1 when the pane can't be detached. */
i32  app_detach_pane_to_tab(AppState *app, i32 tab_index, i32 pane, i32 insert_at);

/* Returns the pane index at framebuffer point `(x, y)`, or -1 outside
 * the tab's terminal area. Handles both two-pane splits and the
 * multi-pane layout tree. */
i32  app_pane_index_at(AppState *app, const Tab *tab, f32 x, f32 y);
i32  app_pane_drop_zone_at(AppState *app, const Tab *tab,
                           i32 pane, f32 x, f32 y);

/* Where a Cmd-dragged pane lands. Anything outside 1..5 means "no drop". */
enum {
    PANE_DROP_NONE   = 0,
    PANE_DROP_LEFT   = 1,
    PANE_DROP_RIGHT  = 2,
    PANE_DROP_TOP    = 3,
    PANE_DROP_BOTTOM = 4,
    PANE_DROP_SWAP   = 5,   /* dropped in the middle: trade places */
};

/* Resolve a pointer position to the pane + zone a Cmd-drag would drop into.
 *
 * Unlike app_pane_index_at this never demands a pixel-exact hit: a point in a
 * divider gap, in the window padding, over the sidebar or below the last pane
 * snaps to the NEAREST pane instead of resolving to "nothing". That is what
 * makes the gesture feel like it aims at panes rather than at rectangles.
 * Returns false when the point is too far from the tab's pane area to mean
 * anything, or when the only candidate is `src` itself. */
bool app_pane_drop_target_at(AppState *app, const Tab *tab, i32 src,
                             f32 x, f32 y, i32 *out_pane, i32 *out_zone);

/* Resolve a tab's pane index to its Session / Terminal slot. Returns the
 * tab's primary slot for pane 0 or out-of-range indices (single-pane tabs). */
Session  *app_pane_session(const Tab *tab, i32 pane);
Terminal *app_pane_terminal(const Tab *tab, i32 pane);

/* Move a pane's layout leaf to one side of another pane.
 * zone: PANE_DROP_LEFT/RIGHT/TOP/BOTTOM re-parent the leaf; PANE_DROP_SWAP
 * trades the two panes' contents in place. The re-parenting variants change
 * the split tree, not the Terminal+Session slot ownership. */
void app_move_pane_to_zone(AppState *app, Tab *tab, i32 src, i32 dst, i32 zone);

/* Swap two panes' Terminal + Session + render cache slots in place.
 * Retained for command/palette compatibility; Cmd+drag uses layout move. */
void app_swap_panes(AppState *app, Tab *tab, i32 a, i32 b);
Terminal *app_focused_terminal(AppState *app);
/* Open the sidebar's explorer on the active tab's folder: over SFTP for a
 * connected SSH tab, the local ladder otherwise (ui.c). */
void app_explorer_open_for_active_tab(AppState *app);
Session  *app_focused_session(AppState *app);
void      app_extract_cmd_history(AppState *app);

/* Best-effort working directory of the active tab's focused LOCAL pane, for a
 * new tab/split to open in the same folder. OSC 7 first, proc_pidinfo fallback;
 * returns NULL for SSH panes or when the cwd can't be determined (caller then
 * falls back to the default $HOME landing). Pointer is owned by the terminal/
 * session and only valid until the next poll — copy if you need to retain it. */
const char *app_active_local_cwd(AppState *app);

/* Best-effort working directory for a (terminal, session) pair: the terminal's
 * OSC 7 cwd when the shell emitted one, else the session's process cwd. Returns
 * NULL when neither resolves. Used to scope per-folder command history. */
const char *app_terminal_cwd(Terminal *t, Session *s);
/* Where the file browser opens when nobody named a directory. ONE ladder:
 * Vault graph root, then the focused pane's cwd (OSC 7, then the proc probe),
 * then getcwd() unless it is "/", then $HOME. Shared static buffer. */
const char *app_fb_open_dir(AppState *app);

/* Move `s`'s shell into `dir` by typing "cd '<dir>'" at its prompt, replacing
 * whatever was already on the line. The path is single-quote escaped — a cwd
 * is untrusted terminal input — and the pane is NOT cleared afterwards, unlike
 * the session-restore cd next to it.
 *
 * Returns false, having sent NOTHING (not even the line kill), when the
 * session is not a connected local PTY — an SSH pane's paths belong to the
 * remote host — or when the quoted command would not fit its buffer, since a
 * truncated path is a cd to a different directory.
 *
 * It does NOT check what is running in the pane; the caller decides whether
 * typing into it is the right move (an agent CLI would just receive the text
 * as a prompt). */
bool app_pane_cd_to(Session *s, const char *dir);
SessionType tab_primary_session_type(const Tab *tab);
SessionType tab_secondary_session_type(const Tab *tab);

/* Session save/restore */
bool app_save_sessions(AppState *app);
/* Same, for several windows at once — their tabs are folded into the one tab
 * array the file format has, WITHOUT moving or touching a single tab, so this
 * is safe to call while the app is running (window close, Cmd+Q atexit).
 * Returns false and leaves the existing file alone when no window has tabs. */
bool app_save_sessions_multi(AppState **apps, i32 app_count);

/* Web views alive in this PROCESS — browser tabs plus browser panes, across
 * every window. See ui/browser_budget.h. */
i32  app_browser_view_count(void);
bool app_restore_sessions(AppState *app);
/* Move the saved session aside (sessions.not-restored.json) instead of
 * restoring it; false when nothing was saved. See core/launch_guard.h. */
bool app_sessions_set_aside(char *kept, usize kept_cap);

/* Tab title: returns custom_title if set, otherwise auto title */
const char *tab_effective_title(const Tab *tab);
/* Display name for a ChatTool passed as i32 ("Claude Code", "Codex", …).
 * "Terminal" for anything unrecognised. Exposed for the tab popup, which
 * names the agent that just finished. */
const char *app_agent_display_name(i32 tool);

/* Floating-panel chrome: rounded body + 1px border ring carrying a soft drop
 * shadow (shadow_size = 0 for a flush panel). The shared design language for
 * toasts, menus and overlays — use it instead of hand-rolling a lookalike. */
void ui_draw_panel_chrome(Renderer *r, f32 x, f32 y, f32 w, f32 h,
                          Color body, Color border, f32 radius, f32 dpi,
                          f32 shadow_size, f32 shadow_alpha);
void tab_format_display_title(const Tab *tab, char *buf, i32 buf_size);

/* Group chip width */
f32 app_get_group_chip_width(const AppState *app, i32 gi);

/* Sidebar sections (ui/sidebar_layout.h). */
enum { SIDE_ROW_TAB = 0, SIDE_ROW_GROUP = 1, SIDE_ROW_NEW = 2 };
typedef struct { u8 kind; i32 index; } SideTabRow;   /* index: tab, group, or -1 */
bool app_sidebar_tabs_docked(const AppState *app);   /* config.sidebar_tabs */
bool app_sidebar_rail(const AppState *app);          /* docked and folded to icons this frame */
f32  app_sidebar_target_width(const AppState *app);  /* what the open/close slide animates to */
/* The docked list's rows in display order (the strip's order); NULL/0 counts. */
i32  app_side_tab_rows(const AppState *app, SideTabRow *rows, i32 cap);
/* Every pane in the window with a known foreground agent (the Agents section). */
i32  app_side_agent_count(const AppState *app);
/* A section header's action icons (ICON_NONE when absent). Packed flush RIGHT
 * with action 1 OUTERMOST and action 2 to its left — the painter draws them
 * that way and the hit test reads them that way, from this one answer. */
i32  app_side_section_actions(i32 section, i32 *act1, i32 *act2);   /* returns the count */
/* Section rects for THIS frame's sidebar geometry — render, hit test and the
 * tab popup all read the same answer. */
void app_sidebar_layout(const AppState *app, SidebarLayout *out);
/* The rail's buttons, top to bottom, as the painter lays them and the hit
 * test reads them: kind SIDE_ROW_TAB/GROUP/NEW as above, plus the one below. */
enum { SIDE_RAIL_SECTION = 3 };
typedef struct { u8 kind; i32 index; f32 y, h; } SideRailItem;
i32  app_side_rail_items(const AppState *app, f32 y, f32 h, SideRailItem *out, i32 cap);

/* Start inline rename for a tab */
void app_start_tab_rename(AppState *app, i32 tab_index);

/* The tab context menu's rows, top to bottom: the ONE list its painter, its
 * hover tracker and its click handler all read, so none of them counts rows
 * or switches on a bare row number of its own. */
typedef enum {
    TAB_MENU_RENAME = 0,
    TAB_MENU_CLOSE,
    /* The tab becomes a window of its own. The same thing dragging it out of
     * the window does, for where that gesture has nowhere to go: a fullscreen
     * window has no outside, and tabs docked in the sidebar are not dragged. */
    TAB_MENU_NEW_WINDOW,
    TAB_MENU_GROUP_ADD,
    TAB_MENU_GROUP_REMOVE,
} TabMenuRow;
#define TAB_MENU_MAX_ROWS 5
/* Fills `rows` for the tab the menu was opened on; returns how many. Grouped
 * top to bottom: the title | where the tab lives | closing it, last. */
i32  app_tab_menu_rows(const AppState *app, TabMenuRow *rows, i32 cap);
/* ...laid out: the box in framebuffer px, the rows, and the top of each (the
 * hairlines between its groups already counted). Width and placement are the
 * shared context menu's (ui/ctx_menu.h). Returns the row count. */
typedef struct {
    f32        x, y, w, h;
    i32        count;
    TabMenuRow kinds[TAB_MENU_MAX_ROWS];
    bool       sep_after[TAB_MENU_MAX_ROWS];
    f32        row_y[TAB_MENU_MAX_ROWS];
    f32        row_h;
} TabMenuLayout;
i32  app_tab_menu_layout(const AppState *app, TabMenuLayout *out);
/* The row under a point, or -1 (outside, padding, a hairline's gap). */
i32  app_tab_menu_row_at(const AppState *app, f32 x, f32 y);

/* The tab-group chip's menu: a strip of colour swatches over five rows. One
 * layout for its painter, its hover tracker and its click handler — they each
 * carried the same eleven lines of arithmetic. */
typedef enum {
    GROUP_MENU_RENAME = 0,
    GROUP_MENU_NEW_TAB,
    GROUP_MENU_TOGGLE,      /* Close Group / Open Group */
    GROUP_MENU_UNGROUP,
    GROUP_MENU_DELETE,      /* closes every tab in the group: the danger row */
    GROUP_MENU_COUNT
} GroupMenuRow;
#define GROUP_MENU_SWATCHES 8
typedef struct {
    f32        x, y, w, h;          /* the whole panel */
    f32        strip_h;             /* the swatch strip, from the panel's top */
    f32        sw_x0, sw_y, sw, sw_gap;   /* first swatch's left, their top, size, gap */
    CtxMenuBox rows;                /* the five rows, under the strip's hairline */
} GroupMenuLayout;
void app_group_menu_layout(const AppState *app, GroupMenuLayout *out);
/* The swatch under a point (0..7) or -1; the row under it or -1. */
i32  app_group_menu_swatch_at(const GroupMenuLayout *l, f32 x, f32 y);

/* Confirm or cancel inline rename */
void app_confirm_tab_rename(AppState *app);
void app_cancel_tab_rename(AppState *app);

/* Inline chip (workspace) rename */
void app_start_chip_rename(AppState *app, i32 group_index);
void app_confirm_chip_rename(AppState *app);
void app_cancel_chip_rename(AppState *app);

/* Tab groups */
i32  app_create_tab_group(AppState *app, const char *name, Color color);
void app_delete_tab_group(AppState *app, i32 group_index);
void app_rename_tab_group(AppState *app, i32 group_index, const char *name);
void app_recolor_tab_group(AppState *app, i32 group_index, Color color);
void app_set_tab_group(AppState *app, i32 tab_index, i32 group_index);
void app_remove_tab_from_group(AppState *app, i32 tab_index);
void app_toggle_tab_group_collapsed(AppState *app, i32 group_index);
void app_close_tab_group(AppState *app, i32 group_index);
i32  app_tab_group_count_tabs(AppState *app, i32 group_index);
/* Ungroup: remove every tab from this group AND delete the group. Tabs
 * remain open. (User-facing label: "Ungroup".) */
void app_ungroup_tab_group(AppState *app, i32 group_index);
/* Persistence: groups (name + color + collapsed) live in
 * ~/.config/Liu/workspaces.json. Tab→group mapping is *not* persisted —
 * sessions don't survive process restart anyway. */
void app_save_workspaces(const AppState *app);
void app_load_workspaces(AppState *app);
/* Built-in 8-color palette accessors */
i32  app_group_palette_size(void);
Color app_group_palette_color(i32 idx);
i32  app_group_palette_match(Color c);  /* nearest palette idx, for menu highlight */

/* Passphrase cache — in-memory only, cleared on exit */
const char *passphrase_cache_lookup(AppState *app, const char *key_path);
void        passphrase_cache_store(AppState *app, const char *key_path, const char *passphrase);

/* Smart Vault — UI helpers (defined in ui.c) */
void app_vault_open_unlock(AppState *app, i32 pending_action);  /* show unlock overlay */
bool app_vault_submit_unlock(AppState *app);   /* called on Enter in unlock dialog */
void app_vault_cancel_unlock(AppState *app);   /* called on Escape */
void app_vault_lock_now(AppState *app);        /* zero DEK, close overlays */
void app_vault_render_unlock(AppState *app);   /* render unlock overlay */
void app_vault_auto_lock_tick(AppState *app);  /* per-frame idle check */

/* Populate SSHConfig secrets from the vault. Reads the three
 * *_secret_id fields on `cfg` and, for each non-empty one, reveals
 * plaintext into the corresponding SSHConfig buffer. Returns:
 *   true       — all requested secrets revealed (or none were requested).
 *   false      — vault is locked or a reveal failed; `*need_unlock_out`
 *                is set to true if the caller should trigger the unlock
 *                flow. The caller owns the SSHConfig buffers in both
 *                cases; plaintext is zeroed automatically on failure. */
bool app_vault_populate_ssh_secrets(AppState *app, SSHConfig *cfg,
                                    bool *need_unlock_out);

/* Smart Vault — browser overlay (vault_ui.c, M5) */
void app_vault_browser_open(AppState *app);
void app_vault_browser_close(AppState *app);
void app_vault_browser_render(AppState *app);
bool app_vault_browser_handle_key(AppState *app, u32 key, u32 mods);
bool app_vault_browser_handle_char(AppState *app, u32 codepoint);

/* Git status — refreshes the focused tab's git_status cache by spawning
 * `git -C <cwd> …`, where <cwd> is the FOCUSED PANE's directory: in a split,
 * the bar describes the pane being typed into, not pane 0. Runs at most once
 * every ~2 seconds, and immediately when the sampled cwd changes (a cd, or
 * focus moving to another pane). Silent no-op for remote (SSH/Mosh) panes or
 * an empty cwd. Defined in ui/git_status.c. */
void app_git_status_tick(AppState *app);

#endif /* UI_H */
