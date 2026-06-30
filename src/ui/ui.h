/*
 * Liu - UI system
 * Tab management, split panes, sidebar for host/SFTP, themed rendering.
 */
#ifndef UI_H
#define UI_H

#include "core/types.h"
#include "core/config.h"
#include "core/keybind.h"
#include "terminal/terminal.h"
#include "terminal/url.h"
#include "ssh/ssh_session.h"
#include "ssh/keygen.h"
#include "core/sites.h"
#include "vault/vault.h"
#include "renderer/renderer.h"
#include "ui/settings.h"
#include "ui/filebrowser.h"
#include "ui/anim.h"
#include "update/updater.h"
#include "notify/notify_server.h"
#include "translate/translate_segment.h"
#include "translate/translate_history_cleanup.h"

/* Forward decl — full struct lives in core/agent_detect.h. AppState carries
 * a heap-allocated array of these for the Cmd+K → Create Theme picker. */
struct AgentInfo;

#define MAX_TABS 32
#define MAX_TAB_GROUPS 8
#define MAX_SPLIT_PANES 8
#define MAX_SPLIT_NODES (MAX_SPLIT_PANES * 2 - 1)

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
    i32           *glyph_counts;
    i32           *glyph_caps;
    i32            cached_rows;
    i32            cached_cols;
    f32            cached_cw, cached_ch;     /* invalidation: font size change */
    f32            cached_origin_x, cached_origin_y; /* cached cell positions are absolute */
    bool           all_rows_dirty;           /* forces full rebuild next frame */
} TermRenderCache;

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

/* Tab content kind. Default (0) is the historic terminal-with-PTY tab.
 * `TAB_FILEBROWSER` tabs hold a FileBrowser instead — their `terminal`,
 * `session`, `terminal2`, `session2`, and split pane state stay zeroed. */
typedef enum {
    TAB_TERMINAL = 0,
    TAB_FILEBROWSER,
} TabKind;

typedef struct {
    char       title[128];
    char       custom_title[128]; /* user-set title (empty = use auto title) */
    bool       title_locked;      /* if true, OSC title changes don't override */
    /* Last foreground CLI agent detected for this tab (a ChatTool value,
     * stored as i32 to avoid pulling history/event.h into this header).
     * Drives auto-naming the tab after the running agent and detecting when
     * it exits. CHAT_TOOL_UNKNOWN (0) = no agent. */
    i32        last_fg_tool;
    TabKind    kind;              /* TAB_TERMINAL = 0 (default) or TAB_FILEBROWSER */
    /* File-browser tab payload (only when kind == TAB_FILEBROWSER). Heap-
     * allocated so we don't bloat every terminal tab by ~4KB; freed by
     * app_close_tab. The viewer state (view_mode, view_content, …) lives
     * inside FileBrowser already. */
    FileBrowser *fb;
    /* When the FB tab has a file open, the right-side viewer takes this
     * fraction of the tab content area. Stored per-tab so each FB tab
     * remembers its own divider position. 0 = use VIEWER_WIDTH_RATIO. */
    f32        fb_viewer_ratio;
    bool       fb_viewer_only;   /* true for detached Markdown viewer tabs */
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
    SplitAnimRect split_move_from[MAX_SPLIT_PANES];
    /* Kept as -1 sentinels for older session/state paths that still clear
     * them; pane swaps now use split_move_from[] geometry animation. */
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

    /* Sleep state for inactive tabs */
    bool       sleeping;
    bool       sleep_disabled;    /* user opt-out: never auto-sleep this tab */
    f64        last_activity_time;

    /* Timestamp of the last keystroke sent to this tab's PTY session(s). Lets the
     * agent accent-bar gate tell keystroke echo / TUI repaint (lands within a few
     * ms) apart from genuine agent output (arrives with latency) — the bar only
     * lights on output that lands >~150 ms after a keystroke. */
    f64        last_keystroke_time;

    /* Decaying count of recent PTY output bytes (≈ bytes over the last ~0.5 s,
     * exponentially weighted). Distinguishes an agent that is actively working
     * — a sustained burst of output — from one sitting idle at a prompt, whose
     * only output is a small periodic cursor blink. Drives the tab accent
     * animation; see TAB_ACCENT_WORK_THRESHOLD in ui.c. */
    f64        out_accum;
    f64        out_accum_t;
    /* Wall-clock deadline until which the agent accent bar stays "working".
     * Refreshed whenever out_accum crosses the work threshold; the render gate
     * checks `now < agent_work_until` instead of the raw threshold so bursty
     * agent output (token gaps, thinking pauses, tool calls) doesn't blink the
     * bar on/off. See TAB_ACCENT_WORK_HOLD in ui.c. */
    f64        agent_work_until;
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
     * local sessions when the terminal's cwd is inside a git repo.
     * Values are displayed in the status bar as "+N -M ↑K". */
    struct {
        bool   valid;           /* has been polled at least once */
        bool   is_repo;
        i32    added;           /* unstaged + staged line additions */
        i32    removed;         /* unstaged + staged line deletions */
        i32    ahead;           /* commits ahead of upstream */
        i32    files_changed;   /* distinct uncommitted files (porcelain) */
        f64    last_check_ts;   /* platform_time_sec when last polled */
        char  *cached_cwd;      /* heap-alloc'd cwd at time of last poll, NULL until populated */
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

    /* Drag-and-drop live animation state */
    f32        render_x;
    /* Open animation: a new tab fades + slides in. anim_start'd in app_new_tab;
     * the pill render multiplies alpha + offsets x by its eased progress. */
    Anim       tab_open;
} Tab;

/* =========================================================================
 * Sidebar mode
 * ========================================================================= */

typedef enum {
    SIDEBAR_HOSTS,
    SIDEBAR_SFTP,
    SIDEBAR_SNIPPETS,
} SidebarMode;

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
    u8   kind;          /* 0=resume, 1=new-session, 2=view-transcript */
    u8   tool;          /* ChatTool enum for resume/new rows */
    char label[64];
    char command[256];
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
    f32       dpi_scale;

    /* Terminal grid dimensions */
    i32       grid_cols;
    i32       grid_rows;

    /* UI regions (in framebuffer pixels) */
    f32       tab_bar_height;
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
    f32       tab_drag_offset_x;   /* current mouse X offset from start */
    i32       tab_drag_target;     /* insertion target index (-1 if none) */
    bool      tab_drag_pending;    /* mouse down on tab, waiting for threshold */
    bool      tab_drag_into_split; /* cursor entered terminal area -- showing split zones */
    i32       tab_drag_split_zone; /* 0=none 1=left 2=right 3=top 4=bottom */

    /* Cached tab layout (set by render_toolbar, read by event handling) */
    f32       _tab_bar_x;         /* start X of first tab in fb pixels */
    f32       _tab_w;             /* computed tab width in fb pixels */
    f32       _tab_gap;           /* computed tab gap in fb pixels */
    /* Horizontally-scrolling tab strip. Tabs hold a comfortable minimum
     * width (TAB_MIN_W_PT); when more are open than fit, the strip scrolls
     * instead of shrinking further. All set by render_toolbar, read by
     * hittest.c and the EVENT_SCROLL handler in main.c. */
    f32       _tab_scroll_x;      /* current scroll offset (content px scrolled left) */
    f32       _tab_scroll_max;    /* max scroll = max(0, content_w - viewport_w) */
    f32       _tab_view_x;        /* tab viewport left edge (fb px) */
    f32       _tab_view_w;        /* tab viewport width (clip region, fb px) */
    f32       _tab_plus_x;        /* pinned new-tab (+) button left edge (fb px) */
    i32       _tab_scroll_last_active; /* active tab we last auto-scrolled to */

    /* Bell */
    f64       bell_flash_time; /* timestamp of last bell flash (0 = inactive) */
    bool      window_focused;  /* is the app window currently focused? */

    /* Sidebar resize drag */
    bool      sidebar_resizing;
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

    /* Resource monitor */
    f32       res_cpu;       /* CPU % */
    f32       res_mem_mb;    /* RSS in MB */
    f64       res_last_update;

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

    /* Toast notification */
    char      toast_message[256];
    f64       toast_start_time;  /* 0 = inactive, auto-dismiss after 3s */

    /* Previous-run crash banner (populated on launch from crashlog). Persists
     * at the top of the window until the user dismisses it (Esc / click). */
    bool      crash_banner_active;
    char      crash_banner_reason[256];
    char      crash_banner_path[512];
    f32       crash_banner_x, crash_banner_y, crash_banner_w, crash_banner_h; /* last drawn rect (hit-test) */

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
    char      translate_fg_agent_id[AGENT_ID_CAP]; /* agent that owns the prompt */
    char      translate_recent_paste[2048];
    f64       translate_recent_paste_time;
    Terminal *translate_recent_paste_term;
    Session  *translate_recent_paste_sess;
    char      translate_input_shadow[4096];
    i32       translate_input_shadow_len;
    Terminal *translate_input_shadow_term;
    Session  *translate_input_shadow_sess;
    f64       translate_input_shadow_time;
    bool      translate_input_shadow_overflow; /* shadow lost bytes — capture
                                                * must fall back to the grid */
    bool      translate_active;
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
    i32       translate_typed_bs;       /* Backspaces that erase the captured text */
    char      translate_stream_line[1024];
    i32       translate_stream_line_len;
    bool      translate_stream_ready;
    bool      translate_stream_emitted;
    /* Agent-backend progress notifications: track whether we've already
     * announced "receiving from <agent>" so the toast fires once per
     * translation when the child's first stdout byte arrives. */
    bool      translate_agent_first_bytes_announced;
    i32       translate_child_pid;      /* agent backend: fork()ed child */
    i32       translate_stdout_fd;      /* agent backend: pipe read end; -1 idle */
    f64       translate_spawn_time;
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
    /* Segmented translation: when the captured prompt carries KEEP tokens
     * (dropped image paths, URLs, @file refs, [Image #N] chips) the line is
     * split (translate_segment_split) and TEXT segments are translated one
     * at a time while KEEP segments are re-emitted verbatim in their
     * original position. translate_seg_count == 0 → legacy single-shot. */
    TranslateSeg translate_segs[TRANSLATE_MAX_SEGS];
    i32       translate_seg_count;
    i32       translate_seg_cur;
    bool      translate_seg_text_emitted; /* current TEXT seg wrote bytes */
    bool      translate_seg_pending_ws;   /* held-back whitespace run */
    char      translate_seg_scratch[4096];/* NUL-terminated current TEXT seg */

    /* Scrollbar auto-hide timer */
    f64       scrollbar_last_activity;

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
     * chatter from non-visible panes intentionally do not flip this — we
     * don't want random remote output to peg ProMotion at 120 Hz. */
    bool      visible_session_data_this_frame;

    /* Overlays */
    SettingsPanel settings;

    /* Sites / dev-server manager: backend runtime + UI view state. */
    SiteManager   site_mgr;
    SiteManagerUI sites;

    /* In-app auto-updater (checks a feed, verifies + installs new versions). */
    UpdateState   updater;

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
     * Currently surfaces directional split commands; expandable later. */
    bool      term_ctx_menu_active;
    f32       term_ctx_menu_x;
    f32       term_ctx_menu_y;
    i32       term_ctx_menu_selected;

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

    /* Agent resume picker (opened after choosing a session in Agent History).
     * Lists CLI agents installed locally and lets the user pick which one to
     * resume/launch with. Rows are baked in agent_picker_rows[], the kind
     * and target shell command live inside each row. */
    bool      agent_picker_active;
    char      agent_picker_title[96];
    char      agent_picker_session_id[96];
    char      agent_picker_session_path[512];
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

    /* Pending resume/start-new command waiting for the focused session's
     * foreground process group to drain (e.g. a previous Claude CLI exits
     * after SIGTERM) before the new shell command can be pasted. Cleared
     * once the prompt is back or the deadline passes. */
    Session  *pending_agent_session;
    char      pending_agent_command[512];
    f64       pending_agent_deadline_sec;
    bool      pending_agent_active;

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

    /* Search bar (Cmd+F) */
    bool      search_active;
    char      search_query[256];
    i32       search_query_len;

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

    /* SSH connect dialog */
    bool      ssh_dialog_active;
    char      ssh_host[256];
    char      ssh_user[128];
    char      ssh_port[8];
    char      ssh_password[256];
    i32       ssh_field;  /* 0=host, 1=user, 2=port, 3=password */

    /* Command history popup (Option+Up) */
    bool      cmd_history_active;
    char      cmd_history[5][512];       /* sanitized label shown in popup */
    char    (*cmd_history_raw)[4096];    /* heap-allocated [5][4096], original command replayed on Enter */
    i32       cmd_history_count;
    i32       cmd_history_selected; /* 0 = most recent */

    /* Inline autosuggest — fish-style ghost completion that mirrors the most
     * recent folder-history match for the current prompt prefix. Rendered in
     * dim text right after the typing cursor; ACT_ACCEPT_SUGGESTION (default
     * Right arrow) accepts and injects the remaining suffix into the PTY. */
    bool      autosuggest_visible;
    char      autosuggest_full[1024];    /* full history line that matched */
    char      autosuggest_suffix[1024];  /* the portion still un-typed */
    Terminal *autosuggest_target_term;   /* term the suggestion applies to */
    Session  *autosuggest_target_sess;

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

    /* URL hover detection */
    TermURL   hover_url;
    bool      url_hover_active;
    i32       url_hover_col;      /* last cell checked — avoid re-scanning */
    i32       url_hover_row;

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

    /* SSH dialog recent history suggestions. Heap-allocated on first dialog
     * open (~4.7 KB) and freed in app_destroy. Capacity is SSH_HISTORY_CAP. */
    VaultHistoryEntry *ssh_history;     /* [SSH_HISTORY_CAP] when non-NULL */
    i32       ssh_history_count;
    i32       ssh_history_selected;  /* -1 = no selection */
    i32       ssh_history_hover;     /* -1 = no hover; MOUSE_MOVE updates */
    /* Inline error banner shown inside the SSH dialog — set by submit
     * when preconditions fail (e.g. empty password). Cleared on char
     * input so the user sees feedback as they type. */
    char      ssh_dialog_error[160];
    f64       ssh_dialog_error_until;   /* platform_time_sec() expiry */

    /* Create Theme dialog (Cmd+K → Create Theme...). The user types a
     * theme name + a free-form visual description, picks a CLI agent
     * detected from PATH, and Liu spawns the agent in the background to
     * generate the JSON file under ~/.config/Liu/themes/. The same
     * fields drive both the modal UI and the long-running generation
     * task — the modal can be closed while the agent keeps running. */
    bool      create_theme_active;
    /* When true the same dialog/state machine drives the "Create Note" AI
     * doc generator instead of theme generation: the prompt asks for raw
     * Markdown and the finalizer writes <vault>/<slug>.md verbatim. */
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
    i32       create_theme_stdout_fd;     /* read end of the pipe; -1 idle */
    /* Wall-clock at spawn time — drives the elapsed-time readout in the
     * running-phase modal. 0 when no spawn in flight. */
    f64       create_theme_spawn_time;
    /* Cumulative bytes read from the agent's stdout. The log buffer is
     * a rolling tail so its `_len` doesn't reflect total throughput;
     * this counter does, and feeds the "received" indicator. */
    i64       create_theme_total_bytes;
    /* Prompt size in bytes (recorded at spawn time). Used together with
     * total_bytes to surface a rough token estimate in the running modal —
     * a calibrated ~4-bytes-per-token heuristic, since the agents we spawn
     * don't emit structured usage data. */
    i64       create_theme_prompt_bytes;
    char      create_theme_target_path[1024]; /* expected JSON destination */
    /* Captured agent stdout. Heap-allocated (64 KB) so a full theme JSON
     * (~3 KB) plus the agent's chatter fits with margin. Rolling tail —
     * once we hit cap we shift down so the most-recent bytes survive.
     * The renderer only shows the last ~5 lines; the parser scans the
     * whole buffer for the JSON object after the agent exits. */
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
    i32       kbi_num_prompts;
    char    (*kbi_prompts)[256];     /* heap [KBI_MAX_PROMPTS][256] when non-NULL */
    bool      kbi_echo[KBI_MAX_PROMPTS];
    char    (*kbi_responses)[256];   /* heap [KBI_MAX_PROMPTS][256] when non-NULL */
    i32       kbi_field;

    /* File browser */
    FileBrowser  filebrowser;

    /* Sidebar focus — set on any sidebar click, cleared on terminal click.
     * Gates the file-browser keyboard shortcuts (Cmd+C/X/V, Delete, F2, …)
     * so they don't steal keys from the terminal when the user isn't
     * actively working in the sidebar. */
    bool         sidebar_focused;

    /* File clipboard (last Cmd+C/X from a file-browser entry). */
    FileClipboard  file_clipboard;

    /* File-browser right-click context menu */
    bool      fb_ctx_menu_active;
    i32       fb_ctx_menu_entry;      /* entry index the menu was opened on */
    f32       fb_ctx_menu_x;
    f32       fb_ctx_menu_y;
    i32       fb_ctx_menu_selected;   /* hovered row, -1 = none */

    /* Inline prompt for rename / new-folder / paste-name conflicts.
     * mode: 0=rename, 1=new folder. */
    bool      fb_prompt_active;
    u8        fb_prompt_mode;
    i32       fb_prompt_index;        /* entry being renamed (rename mode) */
    char      fb_prompt_buf[256];
    i32       fb_prompt_len;

    /* Transient status line shown at the bottom of the sidebar. */
    char      fb_status[128];
    f64       fb_status_until;        /* absolute wall-clock seconds */

    /* Background paste/transfer task. The worker thread runs a snapshot of
     * the clipboard against a shimmed destination so a GB-scale transfer no
     * longer freezes the UI. Main thread joins the thread after `done`
     * flips true, refreshes the filebrowser, clears state. */
    bool      fb_task_active;           /* thread running or join pending */
    void     *fb_task_opaque;           /* FileOpsPasteTask * (main.c only) */

    /* Drag-and-drop within sidebar: user pressed on an entry and is
     * dragging it. When the mouse is released on another folder row we
     * perform a move. `fb_drag_start_{x,y}` gate the transition from
     * click→drag so a tiny wiggle doesn't trigger a move. */
    bool      fb_drag_active;
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
    char      ime_preedit[256];
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
    i32       vault_browser_edit_kind;         /* VaultSecretKind */
    bool      vault_browser_edit_is_new;       /* true = create, false = update */
    char      vault_browser_edit_secret_id[48];/* for update */
    char      vault_browser_status[128];
    f64       vault_browser_status_ts;

    /* Renderer */
    Renderer  renderer;

    /* Render benchmark (optional). Sample buffer is lazy-allocated on first
     * toggle so the 2 KB ring buffer doesn't sit in BSS for everyone. */
    bool      bench_enabled;
    f64      *bench_samples_ms;       /* ring buffer of per-frame render ms (heap, NULL until first toggle) */
    i32       bench_head;             /* next write index */
    i32       bench_count;            /* valid sample count (<=256) */
    f64       bench_last_frame_t;     /* previous frame completion time */
    f64       bench_fps_accum;        /* accumulator for FPS */
    i32       bench_fps_frames;
    f64       bench_fps_t0;

    /* File-browser <-> terminal cwd auto-sync.
     *
     * When the active tab's local PTY changes its working directory (via
     * OSC 7, or fallback proc-pidinfo if the shell doesn't emit it), the
     * file browser navigates to the new path. We track the path we last
     * synced to so we only act on actual transitions — that way a user's
     * manual fb navigation isn't immediately reverted on every frame.
     * Also tracks the active tab index so switching tabs re-syncs once. */
    char      fb_sync_last_cwd[1024];
    i32       fb_sync_last_tab;
    f64       fb_sync_last_check_ts;
    /* Which terminal the last sync looked at + its OSC 7 seq at that time.
     * Pointer identity only (never dereferenced after free); a recycled
     * address simply triggers one extra immediate sync — safe direction. */
    const Terminal *fb_sync_last_term;
    u64       fb_sync_last_cwd_seq;
} AppState;

/* Init/destroy */
bool app_init(AppState *app, i32 width, i32 height, f32 dpi_scale);
void app_destroy(AppState *app);

/* Tab management */
i32  app_new_tab(AppState *app, Session *session, const char *title);
i32  app_new_filebrowser_tab(AppState *app, const char *path);
/* Refresh `tab->title` from the FB tab's current cwd. Honors HOME (`~`)
 * and root (`/`). Does nothing if the tab has a custom_title. No-op for
 * non-FB tabs or tabs without an FB instance. */
void app_refresh_fb_tab_title(Tab *tab);
/* Foreground AI-agent probe for the close confirm. Returns the running
 * agent's ChatTool as i32 (0 = none) and optionally its display name
 * ("Claude Code", "Codex", …). The tab variant scans every split pane. */
i32  app_tab_running_agent(AppState *app, i32 tab_index, const char **display);
i32  app_pane_running_agent(AppState *app, i32 tab_index, i32 pane, const char **display);
void app_close_tab(AppState *app, i32 index);
void app_switch_tab(AppState *app, i32 index);
bool app_wake_tab(AppState *app, i32 index);
void app_sleep_inactive_tabs(AppState *app, f64 now_sec);
bool app_sleep_tab(AppState *app, i32 index);
void app_show_toast(AppState *app, const char *message);
void app_show_crash_banner(AppState *app, const char *reason, const char *path);
void render_crash_banner(AppState *app);
Tab *app_active_tab(AppState *app);

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
void app_render(AppState *app);

/* Render a terminal grid (primary or scrollback) into a rect at (origin_x,
 * origin_y), using `cache` for per-row dirty tracking. Exposed so the Sites
 * panel can render each site's headless log terminal. */
void render_terminal_pane(AppState *app, Terminal *term, TermRenderCache *cache,
                          f32 origin_x, f32 origin_y, bool focused);

void app_bench_toggle(AppState *app);
void app_bench_record(AppState *app, f64 frame_ms);

/* Process I/O for all sessions. Returns true if any data was received. */
bool app_poll_sessions(AppState *app);

/* Tab reorder */
void app_move_tab(AppState *app, i32 from, i32 to);

/* Split pane */
void app_split_tab(AppState *app, SplitType dir);
void app_resize_tab_panes(AppState *app, Tab *tab);
/* Same as app_split_tab but lets the caller place the *new* pane on the
 * leading edge (left for SPLIT_H, top for SPLIT_V) when `new_first` is true.
 * Used by the right-click context menu's Split Left / Split Up entries. */
void app_split_tab_dir(AppState *app, SplitType dir, bool new_first);
void app_split_tab_from_drag(AppState *app, i32 src_idx, i32 zone);

/* Collapse a split tab back to a single pane by killing one side.
 * `pane` is 0 (left/top) or 1 (right/bottom); the surviving pane is
 * resized to the full tab grid and the dead pane's Terminal/Session
 * are handed to the background trash queue. No-op when not split. */
void app_close_split_pane(AppState *app, i32 tab_index, i32 pane);

/* Returns the pane index under (x, y) in framebuffer pixels, or -1 if
 * the point falls outside the tab's terminal area. Handles both the
 * legacy binary split and the multi-pane layout tree. */
i32  app_pane_index_at(AppState *app, const Tab *tab, f32 x, f32 y);
i32  app_pane_drop_zone_at(AppState *app, const Tab *tab,
                           i32 pane, f32 x, f32 y);

/* Resolve a tab's pane index to its Session / Terminal slot. Returns the
 * tab's primary slot for pane 0 or out-of-range indices (single-pane tabs). */
Session  *app_pane_session(const Tab *tab, i32 pane);
Terminal *app_pane_terminal(const Tab *tab, i32 pane);

/* Move a pane's layout leaf to one side of another pane.
 * zone: 1=left, 2=right, 3=top, 4=bottom. This changes the split tree,
 * not the Terminal+Session slot ownership. */
void app_move_pane_to_zone(AppState *app, Tab *tab, i32 src, i32 dst, i32 zone);

/* Swap two panes' Terminal + Session + render cache slots in place.
 * Retained for command/palette compatibility; Cmd+drag uses layout move. */
void app_swap_panes(AppState *app, Tab *tab, i32 a, i32 b);
Terminal *app_focused_terminal(AppState *app);
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
SessionType tab_primary_session_type(const Tab *tab);
SessionType tab_secondary_session_type(const Tab *tab);

/* Session save/restore */
bool app_save_sessions(AppState *app);
bool app_restore_sessions(AppState *app);

/* Tab title: returns custom_title if set, otherwise auto title */
const char *tab_effective_title(const Tab *tab);
void tab_format_display_title(const Tab *tab, char *buf, i32 buf_size);

/* Group chip width */
f32 app_get_group_chip_width(const AppState *app, i32 gi);

/* Start inline rename for a tab */
void app_start_tab_rename(AppState *app, i32 tab_index);

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
 * remain open. (User-facing label: "Grubu çöz".) */
void app_ungroup_tab_group(AppState *app, i32 group_index);
/* Persistence: groups (name + color + collapsed) live in
 * ~/.config/Liu/workspaces.json. Tab→group mapping is *not* persisted —
 * sessions don't survive process restart anyway. */
void app_save_workspaces(const AppState *app);
void app_load_workspaces(AppState *app);
/* Built-in 8-color palette accessors (Chrome-style) */
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
 * `git -C <cwd> …`. Runs at most once every ~2 seconds. Silent no-op
 * for remote (SSH/Mosh) tabs or empty cwd. Defined in ui/git_status.c. */
void app_git_status_tick(AppState *app);

#endif /* UI_H */
