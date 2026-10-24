/*
 * sftp_tab.h — the SFTP tab: the local disk on the left, a host on the right,
 * and files moved between them.
 *
 * A TAB_SFTP tab owns one SftpTab (Tab.sftp) and ONE Session in Tab.session: a
 * file-transfer connection (SSHConfig.sftp_only — no shell, no PTY), so an
 * account that allows SFTP and nothing else can be used. Keeping the session
 * in Tab.session is what gives it every service the loop gives a connection
 * (the host-key, passphrase and 2FA prompts, the keepalive, the recent-hosts
 * history, the close path); the link policy is driven from sftp_tab_tick,
 * because that pass in app_poll_sessions is written for panes with a terminal.
 *
 * Each pane lists through a FileBrowser (the explorer's own listing, local or
 * SFTP, on its workers) and shows it through a view (ui/sftp_view.h). Where
 * things are is ui/sftp_layout.h; the menu is ui/sftp_menu.h. What a pane does
 * to files — copy, move, rename, make a folder, delete, the prompt that asks
 * first — is the explorer's own code in main.c, run with the pane as its
 * actor (AppState.fb_actor), so the rules about names, overwrites and the
 * Trash are one set.
 */
#ifndef LIU_UI_SFTP_TAB_H
#define LIU_UI_SFTP_TAB_H

#include "core/types.h"
#include "ui/filebrowser.h"
#include "ui/sftp_layout.h"
#include "ui/sftp_menu.h"
#include "ssh/ssh_session.h"

typedef struct AppState AppState;
typedef struct SftpTab SftpTab;

#define SFTP_PANE_LOCAL   0
#define SFTP_PANE_REMOTE  1
#define SFTP_FILTER_CAP   128

/* A pane's footer line has a kind, as the explorer's status line does. */
typedef enum { SFTP_MSG_NONE = 0, SFTP_MSG_INFO, SFTP_MSG_OK, SFTP_MSG_ERROR } SftpMsgKind;

typedef struct {
    FileBrowser  fb;
    /* The view (ui/sftp_view.h): entry indices in the order shown. Rebuilt
     * when the listing, the filter, the order or the hidden switch changes —
     * `view_key` is what it was built from. */
    i32         *view;
    i32         *view_tmp;
    SftpRowKey  *keys;
    i32          view_count, view_cap;
    struct { u64 nav; i32 entries; const void *ptr; u32 stat_gen; } view_key;
    bool         view_dirty;

    char         filter[SFTP_FILTER_CAP];
    i32          filter_len;
    bool         filter_open;          /* the field is shown (it has the keyboard while te_owner is it) */
    SftpCol      sort;
    bool         sort_desc;
    bool         show_hidden;
    SftpHistory  hist;

    f32          scroll_px;
    i32          cursor;               /* view row of the keyboard cursor, -1 none */
    i32          anchor;               /* view row a Shift range starts from */
    f64          sb_active_t;          /* the overlay scrollbar's clock */
    f32          sb_seen;

    char         msg[192];
    u8           msg_kind;             /* SftpMsgKind */
    f64          msg_until;            /* 0 = until replaced */

    SftpCrumbRow crumbs;               /* the breadcrumb as last drawn */
    char         home[FB_MAX_PATH];    /* where "Go to Home Folder" goes */
} SftpPane;

struct SftpTab {
    u64          id;                   /* names the tab to work that outlives a frame */
    SftpPane     pane[2];
    i32          focus;                /* the pane with the keyboard */
    f32          ratio;                /* the left pane's share, 0 = even */
    bool         divider_drag;

    char         host[256];            /* the host as typed / resolved */
    char         label[320];           /* user@host[:port] — the remote pane's name */
    char         name[256];            /* the tab's title: the host as it was asked for */
    char         want_remote[FB_MAX_PATH];   /* where the remote pane goes once up ("" = home) */
    SessionStatus prev_status;
    bool         was_up;               /* the link has been up at least once */
    char         link_msg[192];        /* what the remote pane says while it is not listing */
    u8           link_kind;            /* SftpMsgKind */
    f64          retry_at;             /* an automatic attempt is due then (0 = none) */
    u32          reconnects;           /* attempts the Reconnect button started */

    /* A drag of rows: armed on the press, live past the threshold. */
    bool         drag_armed, drag_live;
    i32          drag_pane;
    f32          drag_x0, drag_y0;
    i32          drag_count;
    char         drag_name[256];       /* the first row's name, for the ghost */
    bool         drag_first_dir;
    i32          drop_pane;            /* -1 none */
    i32          drop_row;             /* a folder row of drop_pane, -1 = its folder */

    /* The menu (Actions, or a right click). */
    bool         menu_open;
    i32          menu_pane;
    i32          menu_sel;
    bool         menu_by_key;
    f32          menu_x, menu_y;
    bool         menu_from_button;

    /* A click on a row: a second within the double-click time opens it. */
    f64          click_t;
    i32          click_pane, click_row;

    SftpGeom     geom;                 /* as laid out by the last paint */
    bool         geom_valid;

    /* Files dropped from the Finder: one event per file, gathered here and
     * sent as one transfer on the next tick (one runs at a time). */
    char       **queue;
    i32          queue_n, queue_cap;
    i32          queue_to;
    char         queue_dest[FB_MAX_PATH * 2];
};

/* The live tab with this id in `app`, or NULL (it closed, or moved to another
 * window). */
SftpTab *sftp_tab_find(AppState *app, u64 id, i32 *index_out);
/* The FileBrowser of a pane, for the explorer's actions. NULL when gone. */
FileBrowser *sftp_tab_actor_fb(AppState *app, u64 id, i32 pane);

/* Open a tab for `session` (a file-transfer connection, owned by the tab from
 * here — destroyed with the tab, or here when the tab cannot be made).
 * `name` is the tab's title (the host as the user named it; "" = `label`),
 * `label` names the far side in full (user@host:port, the remote pane's
 * header); `local_dir` / `remote_dir` are where the panes start ("" = home).
 * The index of the new tab, or -1. */
i32  app_open_sftp_tab(AppState *app, Session *session, const char *name, const char *label,
                       const char *local_dir, const char *remote_dir);
/* For a host tab, use the requested SSH config alias when present, or
 * the host name otherwise. Include a non-default port; the pane header
 * displays the username. */
void sftp_tab_name_for(char *out, usize cap, const char *asked, const char *host, i32 port);
/* Free a tab's SftpTab (not its session: the close path owns that). */
void sftp_tab_destroy(SftpTab *st);

/* Once per poll for every TAB_SFTP tab of the window, active or not: the link
 * policy, the remote pane's wiring on the up / down edges, the views. True
 * when something the window shows changed. */
bool sftp_tab_tick(AppState *app, i32 tab_index, f64 now);
/* True while something in the tab moves on the clock (a connecting spinner). */
bool sftp_tab_animating(const AppState *app);

void sftp_tab_render(AppState *app, SftpTab *st, f32 x, f32 y, f32 w, f32 h);
/* The menu and the drag's ghost, drawn above every panel of the window. */
void sftp_tab_render_overlay(AppState *app);

/* Events, for the active tab when it is a TAB_SFTP. Each answers whether it
 * took the event. `clicks` counts a multi-click. */
bool sftp_tab_mouse_down(AppState *app, f32 x, f32 y, i32 button, u32 mods);
bool sftp_tab_mouse_move(AppState *app, f32 x, f32 y);
bool sftp_tab_mouse_up(AppState *app, f32 x, f32 y, i32 button, u32 mods);
bool sftp_tab_scroll(AppState *app, f32 x, f32 y, f32 dy, bool precise);
bool sftp_tab_key(AppState *app, i32 key, u32 mods);
bool sftp_tab_char(AppState *app, u32 cp, u32 mods);
/* Files dropped from the Finder onto the tab. */
bool sftp_tab_drop_files(AppState *app, f32 x, f32 y, const char *const *paths, i32 count);
/* The pointer's shape over the tab (a CursorType): the divider resizes, the
 * open Filter field is text, the rest is the arrow. */
i32  sftp_tab_cursor(AppState *app, f32 x, f32 y);
/* A menu is open in the active SFTP tab (the loop's gates ask). */
bool sftp_tab_menu_open(const AppState *app);
/* The footer line of a pane, set by the explorer's actions run for it. */
void sftp_tab_say(AppState *app, u64 id, i32 pane, SftpMsgKind kind, const char *msg);

/* ---- main.c: the explorer's file operations, run for a pane ------------- */

/* The selection of pane `from` (or row `entry` when nothing is selected),
 * copied — or moved — into `dest_dir` on pane `to`'s side. */
bool app_sftp_transfer(AppState *app, SftpTab *st, i32 from, i32 entry, i32 to,
                       const char *dest_dir, bool move);
/* Files from the Finder into pane `to`'s `dest_dir`. */
bool app_sftp_transfer_paths(AppState *app, SftpTab *st, const char *const *paths, i32 count,
                             i32 to, const char *dest_dir);
/* A menu row of the explorer's (copy, cut, paste, rename, new folder,
 * delete), done to pane `pane` of the tab. */
void app_sftp_fb_action(AppState *app, SftpTab *st, i32 pane, i32 fb_act, i32 entry);
/* Rows of a pane moved into a folder of the same pane (a drag onto it). */
void app_sftp_move_into(AppState *app, SftpTab *st, i32 pane, i32 entry_dir);
/* Is a transfer running in this window (the explorer's paste task)? */
bool app_sftp_busy(const AppState *app);

#endif
