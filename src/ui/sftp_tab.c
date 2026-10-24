/*
 * Liu — the SFTP tab. See sftp_tab.h for what it is and what it reuses.
 */
#include "ui/sftp_tab.h"
#include "ui/ui.h"
#include "ui/icon.h"
#include "ui/anim.h"
#include "ui/status_colors.h"
#include "ui/syntax.h"
#include "ui/keycap.h"
#include "ui/textedit.h"
#include "ui/chrome_palette.h"
#include "ui/notification.h"
#include "ui/sftp_ink.h"
#include "core/utf8.h"
#include "core/keybind.h"
#include "platform/platform.h"
#include "ssh/ssh_session.h"
#include "ssh/link_policy.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static u64 g_sftp_next_id = 1;

#define SFTP_DOUBLE_CLICK_S  0.40
#define SFTP_DRAG_START_PT   5.0f
#define SFTP_MSG_HOLD_S      6.0

/* ---- finding a tab ------------------------------------------------------- */

SftpTab *sftp_tab_find(AppState *app, u64 id, i32 *index_out) {
    if (index_out) *index_out = -1;
    if (!app || !id) return NULL;
    for (i32 i = 0; i < app->tab_count; i++) {
        Tab *t = &app->tabs[i];
        if (t->kind == TAB_SFTP && t->sftp && t->sftp->id == id) {
            if (index_out) *index_out = i;
            return t->sftp;
        }
    }
    return NULL;
}

FileBrowser *sftp_tab_actor_fb(AppState *app, u64 id, i32 pane) {
    SftpTab *st = sftp_tab_find(app, id, NULL);
    return (st && pane >= 0 && pane < 2) ? &st->pane[pane].fb : NULL;
}

static SftpTab *active_sftp(const AppState *app) {
    if (!app || app->active_tab < 0 || app->active_tab >= app->tab_count) return NULL;
    const Tab *t = &app->tabs[app->active_tab];
    return (t->kind == TAB_SFTP) ? t->sftp : NULL;
}

/* ---- the pane's footer line ---------------------------------------------- */

static void pane_say(SftpPane *p, SftpMsgKind kind, const char *msg) {
    if (!p) return;
    snprintf(p->msg, sizeof p->msg, "%s", msg ? msg : "");
    /* A name cut by the buffer must not end in half a character. */
    usize n = strlen(p->msg), i = n;
    while (i > 0 && ((u8)p->msg[i - 1] & 0xC0) == 0x80) i--;
    if (i > 0) {
        u8 lead = (u8)p->msg[i - 1];
        usize need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
        if (need > 1 && n - (i - 1) < need) p->msg[i - 1] = '\0';
    }
    p->msg_kind = (u8)kind;
    p->msg_until = kind == SFTP_MSG_ERROR ? 0.0 : platform_time_sec() + SFTP_MSG_HOLD_S;
}

void sftp_tab_say(AppState *app, u64 id, i32 pane, SftpMsgKind kind, const char *msg) {
    SftpTab *st = sftp_tab_find(app, id, NULL);
    if (st && pane >= 0 && pane < 2) pane_say(&st->pane[pane], kind, msg);
}

/* ---- the view ------------------------------------------------------------ */

static i32 pane_stat_known(const FileBrowser *fb) {
    i32 n = 0;
    for (i32 i = 0; i < fb->entry_count; i++) if (fb->entries[i].stat_known) n++;
    return n;
}

static bool pane_grow(SftpPane *p, i32 need) {
    if (need <= p->view_cap) return true;
    i32 cap = p->view_cap ? p->view_cap : 64;
    while (cap < need) cap *= 2;
    i32 *v = realloc(p->view, (usize)cap * sizeof *v);
    if (!v) return false;
    p->view = v;
    i32 *t = realloc(p->view_tmp, (usize)cap * sizeof *t);
    if (!t) return false;
    p->view_tmp = t;
    SftpRowKey *k = realloc(p->keys, (usize)cap * sizeof *k);
    if (!k) return false;
    p->keys = k;
    p->view_cap = cap;
    return true;
}

static const FileEntry *pane_entry(const SftpPane *p, i32 row) {
    if (row < 0 || row >= p->view_count) return NULL;
    i32 ei = p->view[row];
    return (ei >= 0 && ei < p->fb.entry_count) ? &p->fb.entries[ei] : NULL;
}

static bool entry_is_up(const FileEntry *e) { return e && e->name && !strcmp(e->name, ".."); }

/* The view row that shows entry `ei`, or -1. */
static i32 pane_row_of(const SftpPane *p, i32 ei) {
    for (i32 r = 0; r < p->view_count; r++) if (p->view[r] == ei) return r;
    return -1;
}

/* Rebuild when what the view was built from changed. True when it did. */
static bool pane_view_sync(SftpPane *p) {
    FileBrowser *fb = &p->fb;
    i32 known = pane_stat_known(fb);
    bool same = !p->view_dirty && p->view_key.nav == fb->nav_generation &&
                p->view_key.entries == fb->entry_count && p->view_key.ptr == (const void *)fb->entries &&
                p->view_key.stat_gen == (u32)known;
    if (same) return false;
    /* The cursor is kept by NAME across a rebuild of the same folder. */
    char cur_name[256] = "";
    bool same_folder = p->view_key.nav == fb->nav_generation;
    const FileEntry *ce = pane_entry(p, p->cursor);
    if (same_folder && ce && ce->name) snprintf(cur_name, sizeof cur_name, "%s", ce->name);

    i32 n = fb->entry_count;
    if (n > 0 && !pane_grow(p, n)) { p->view_count = 0; return true; }
    for (i32 i = 0; i < n; i++) {
        const FileEntry *e = &fb->entries[i];
        p->keys[i] = (SftpRowKey){ e->name, e->size, e->mtime, e->permissions, e->is_dir, e->stat_known };
    }
    p->view_count = n > 0 ? sftp_view_build(p->keys, n, p->filter, p->show_hidden, p->sort,
                                           p->sort_desc, p->view, p->view_tmp) : 0;
    p->view_key.nav = fb->nav_generation;
    p->view_key.entries = n;
    p->view_key.ptr = fb->entries;
    p->view_key.stat_gen = (u32)known;
    p->view_dirty = false;

    p->cursor = -1;
    if (cur_name[0]) {
        for (i32 r = 0; r < p->view_count; r++) {
            const FileEntry *e = pane_entry(p, r);
            if (e && e->name && !strcmp(e->name, cur_name)) { p->cursor = r; break; }
        }
    }
    /* A listing that came back with a row to show (a rename, a new folder:
     * fb_refresh_keep) hands it over as the browser's primary selection: it
     * becomes this pane's cursor and its only selection. */
    if (fb->selected >= 0 && fb->selected < n) {
        i32 r = pane_row_of(p, fb->selected);
        if (r >= 0) {
            fb_sel_clear(fb);
            fb_sel_set(fb, fb->selected, true);
            p->cursor = r;
            p->anchor = r;
        }
        fb->selected = -1;
    }
    if (!same_folder) { p->anchor = -1; p->scroll_px = 0.0f; }
    return true;
}

static void pane_select_only(SftpPane *p, i32 row) {
    fb_sel_clear(&p->fb);
    const FileEntry *e = pane_entry(p, row);
    if (e && !entry_is_up(e)) fb_sel_set(&p->fb, p->view[row], true);
}

static void pane_select_range(SftpPane *p, i32 a, i32 b) {
    fb_sel_clear(&p->fb);
    if (a > b) { i32 t = a; a = b; b = t; }
    for (i32 r = a; r <= b; r++) {
        const FileEntry *e = pane_entry(p, r);
        if (e && !entry_is_up(e)) fb_sel_set(&p->fb, p->view[r], true);
    }
}

/* How many rows the selection covers, and their bytes (files only). */
static i32 pane_selected(const SftpPane *p, u64 *bytes) {
    i32 n = 0;
    u64 b = 0;
    for (i32 r = 0; r < p->view_count; r++) {
        i32 ei = p->view[r];
        if (!fb_sel_has(&p->fb, ei)) continue;
        n++;
        const FileEntry *e = &p->fb.entries[ei];
        if (!e->is_dir && e->stat_known) b += e->size;
    }
    if (bytes) *bytes = b;
    return n;
}

/* ---- the remote pane's connection ----------------------------------------- */

static bool remote_ready(const AppState *app, const Tab *tab) {
    (void)app;
    return tab && tab->session && session_status(tab->session) == SESSION_CONNECTED &&
           session_sftp_handle(tab->session) != NULL && tab->sftp &&
           tab->sftp->pane[SFTP_PANE_REMOTE].fb.source == FB_SOURCE_SFTP;
}

static bool pane_ready(const AppState *app, const Tab *tab, i32 pane) {
    return pane == SFTP_PANE_LOCAL ? true : remote_ready(app, tab);
}

/* Go to `path` in a pane; `visit` records it in back / forward. */
static bool pane_go(AppState *app, Tab *tab, i32 pane, const char *path, bool visit) {
    SftpTab *st = tab->sftp;
    SftpPane *p = &st->pane[pane];
    if (!path || !path[0]) return false;
    bool ok;
    if (pane == SFTP_PANE_LOCAL) {
        ok = fb_navigate(&p->fb, path);
    } else {
        if (!remote_ready(app, tab) && !(tab->session && session_status(tab->session) == SESSION_CONNECTED))
            return false;
        p->fb.session = tab->session;
        ok = fb_navigate_sftp(&p->fb, session_sftp_handle(tab->session), path);
    }
    if (!ok) { pane_say(p, SFTP_MSG_ERROR, "That folder could not be opened"); return false; }
    if (visit) sftp_hist_visit(&p->hist, path);
    p->filter[0] = '\0';
    p->filter_len = 0;
    p->filter_open = false;
    if (app->te_owner == (const void *)p->filter) app->te_owner = NULL;
    p->cursor = -1;
    p->anchor = -1;
    p->scroll_px = 0.0f;
    p->view_dirty = true;
    return true;
}

static void pane_up(AppState *app, Tab *tab, i32 pane) {
    SftpPane *p = &tab->sftp->pane[pane];
    if (!p->fb.cwd[0] || !strcmp(p->fb.cwd, "/")) return;
    char parent[FB_MAX_PATH], from[256];
    const char *leaf = strrchr(p->fb.cwd, '/');
    snprintf(from, sizeof from, "%s", leaf ? leaf + 1 : "");
    sftp_path_parent(p->fb.cwd, parent, sizeof parent);
    if (pane_go(app, tab, pane, parent, true) && from[0]) {
        /* Coming up out of a folder leaves the cursor on it. fb_refresh_keep's
         * reveal is for a refresh; a navigate is matched here once listed. */
        snprintf(p->fb.reveal_path, sizeof p->fb.reveal_path, "%s/%s",
                 strcmp(parent, "/") ? parent : "", from);
        p->fb.reveal_armed = true;
        p->fb.reveal_nav_generation = p->fb.nav_generation;
        /* A fast local listing has landed inside fb_navigate already. */
        for (i32 i = 0; i < p->fb.entry_count; i++) {
            if (p->fb.entries[i].name && !strcmp(p->fb.entries[i].name, from)) {
                p->fb.selected = i;
                p->fb.reveal_armed = false;
                break;
            }
        }
    }
}

/* ---- create / destroy ---------------------------------------------------- */

static void pane_init(SftpPane *p) {
    memset(p, 0, sizeof *p);
    fb_init(&p->fb);
    p->fb.open = true;
    p->fb.selected = -1;
    p->cursor = -1;
    p->anchor = -1;
    p->sort = SFTP_COL_NAME;
    sftp_hist_reset(&p->hist);
    p->view_dirty = true;
}

static void pane_free(SftpPane *p) {
    fb_destroy(&p->fb);
    free(p->view);
    free(p->view_tmp);
    free(p->keys);
    p->view = p->view_tmp = NULL;
    p->keys = NULL;
    p->view_cap = p->view_count = 0;
}

static void queue_clear(SftpTab *st) {
    for (i32 i = 0; i < st->queue_n; i++) free(st->queue[i]);
    st->queue_n = 0;
}

void sftp_tab_destroy(SftpTab *st) {
    if (!st) return;
    pane_free(&st->pane[0]);
    pane_free(&st->pane[1]);
    queue_clear(st);
    free(st->queue);
    free(st);
}

/* A modal of the app's own is up: the tab takes no key, click or drop. */
static bool app_modal_open(const AppState *a) {
    return a->settings.open || a->palette_active || a->ssh_dialog_active || a->fb_prompt_active ||
           a->fb_ctx_menu_active || a->kbi_dialog_active || a->hostkey_dialog_active ||
           a->passphrase_dialog_active || a->close_confirm_active || a->paste_confirm_active ||
           a->vault_unlock_dialog_active || a->vault_browser_active || a->key_manager_active ||
           a->sites.active || a->sketch.open || a->create_theme_active || a->port_forward_dialog_active ||
           a->transcript_viewer_active || a->agent_picker_active || a->broadcast_overlay_active ||
           a->known_hosts_open || a->lightbox.open || a->tab_rename_active || a->cmd_history_active ||
           a->tab_ctx_menu_active || a->term_ctx_menu_active || a->group_ctx_menu_active;
}

void sftp_tab_name_for(char *out, usize cap, const char *asked, const char *host, i32 port) {
    if (!out || !cap) return;
    if (asked && asked[0] && (!host || strcmp(asked, host) != 0)) snprintf(out, cap, "%s", asked);
    else if (port > 0 && port != 22) snprintf(out, cap, "%s:%d", host ? host : "", (int)port);
    else snprintf(out, cap, "%s", host ? host : "");
}

i32 app_open_sftp_tab(AppState *app, Session *session, const char *name, const char *label,
                      const char *local_dir, const char *remote_dir) {
    if (!app || !session) return -1;
    if (app->tab_count >= MAX_TABS) {
        session_destroy(session);
        app_show_toast(app, "Too many tabs open: close one to open an SFTP tab");
        return -1;
    }
    SftpTab *st = calloc(1, sizeof *st);
    if (!st) { session_destroy(session); app_show_toast(app, "The SFTP tab could not be opened"); return -1; }
    st->id = g_sftp_next_id++;
    pane_init(&st->pane[0]);
    pane_init(&st->pane[1]);
    st->focus = SFTP_PANE_REMOTE;
    st->drop_pane = -1;
    st->drop_row = -1;
    st->prev_status = SESSION_DISCONNECTED;
    snprintf(st->label, sizeof st->label, "%s", label && label[0] ? label : "Remote");
    snprintf(st->name, sizeof st->name, "%s", name && name[0] ? name : st->label);
    snprintf(st->want_remote, sizeof st->want_remote, "%s", remote_dir ? remote_dir : "");
    snprintf(st->link_msg, sizeof st->link_msg, "Connecting to %s\xe2\x80\xa6", st->label);
    st->link_kind = SFTP_MSG_INFO;

    const char *home = getenv("HOME");
    const char *ldir = local_dir && local_dir[0] ? local_dir : (home && home[0] ? home : "/");
    snprintf(st->pane[0].home, sizeof st->pane[0].home, "%s", home && home[0] ? home : "/");
    if (!fb_navigate(&st->pane[0].fb, ldir) && strcmp(ldir, st->pane[0].home))
        (void)fb_navigate(&st->pane[0].fb, st->pane[0].home);
    sftp_hist_visit(&st->pane[0].hist, st->pane[0].fb.cwd[0] ? st->pane[0].fb.cwd : ldir);

    i32 idx = app->tab_count++;
    Tab *tab = &app->tabs[idx];
    memset(tab, 0, sizeof(*tab));
    tab->swap_flash_slot_a = -1;
    tab->swap_flash_slot_b = -1;
    tab->kind          = TAB_SFTP;
    tab->sftp          = st;
    tab->session       = session;
    tab->browser_pane  = -1;
    tab->active        = true;
    tab->dirty         = true;
    tab->group_index   = -1;
    tab->split         = SPLIT_NONE;
    tab->split_ratio   = 0.5f;
    tab->split_pane_count = 1;
    tab->split_root    = -1;
    tab->active_pane   = 0;
    tab->profile_index = -1;
    tab->sleep_disabled = true;
    tab->last_activity_time = platform_time_sec();
    tab->prev_status   = session_status(session);
    snprintf(tab->title, sizeof tab->title, "%s", st->name);
    app->broadcast_targets[idx][0] = false;
    app->broadcast_targets[idx][1] = false;
    app->active_tab = idx;
    return idx;
}

/* ---- the clock ----------------------------------------------------------- */

bool sftp_tab_animating(const AppState *app) { (void)app; return false; }

bool sftp_tab_tick(AppState *app, i32 ti, f64 now) {
    if (!app || ti < 0 || ti >= app->tab_count) return false;
    Tab *tab = &app->tabs[ti];
    SftpTab *st = tab->sftp;
    if (tab->kind != TAB_SFTP || !st) return false;
    Session *s = tab->session;
    bool changed = false;
    SftpPane *rp = &st->pane[SFTP_PANE_REMOTE];

    if (s) {
        i32 retry_ms = 0, attempt = 0;
        SessionLinkEvent ev = session_link_step(s, &retry_ms, &attempt);
        char m[192];
        switch (ev) {
        case SESSION_LINK_CONNECTING:
            snprintf(st->link_msg, sizeof st->link_msg, "Connecting to %s\xe2\x80\xa6", st->label);
            st->link_kind = SFTP_MSG_INFO;
            changed = true;
            break;
        case SESSION_LINK_UP:
        case SESSION_LINK_RECONNECTED:
            st->link_msg[0] = '\0';
            st->link_kind = SFTP_MSG_NONE;
            st->retry_at = 0.0;
            pane_say(rp, SFTP_MSG_OK, ev == SESSION_LINK_UP ? "Connected" : "Connected again");
            changed = true;
            break;
        case SESSION_LINK_LOST: {
            /* The words the transport chose, unless they only say the same. */
            const char *why = session_link_reason(s);
            bool plain = !why[0] || !strcmp(why, "SSH connection lost") || strstr(why, "keepalive");
            if (plain)
                snprintf(m, sizeof m, "The connection was lost. Trying again (%d of %d)\xe2\x80\xa6",
                         (int)attempt, (int)LINK_RETRY_MAX);
            else
                snprintf(m, sizeof m, "The connection was lost: %s. Trying again (%d of %d)\xe2\x80\xa6",
                         why, (int)attempt, (int)LINK_RETRY_MAX);
            snprintf(st->link_msg, sizeof st->link_msg, "%s", m);
            st->link_kind = SFTP_MSG_ERROR;
            st->retry_at = now + retry_ms / 1000.0;
            changed = true;
            break;
        }
        case SESSION_LINK_FAILED:
        case SESSION_LINK_GAVE_UP:
        case SESSION_LINK_ENDED:
            /* Under the title "Not connected": the transport's words, or at
             * least which host it was. */
            if (session_link_reason(s)[0])
                snprintf(st->link_msg, sizeof st->link_msg, "%s", session_link_reason(s));
            else if (ev == SESSION_LINK_GAVE_UP)
                snprintf(st->link_msg, sizeof st->link_msg, "%s did not come back", st->label);
            else if (ev == SESSION_LINK_ENDED)
                snprintf(st->link_msg, sizeof st->link_msg, "%s closed the connection", st->label);
            else
                snprintf(st->link_msg, sizeof st->link_msg, "The connection to %s could not be made", st->label);
            st->link_kind = SFTP_MSG_ERROR;
            st->retry_at = 0.0;
            changed = true;
            break;
        default: break;
        }

        SessionStatus cur = session_status(s);
        if (cur == SESSION_KBI_PENDING && st->prev_status != SESSION_KBI_PENDING) {
            snprintf(st->link_msg, sizeof st->link_msg, "The server is asking a question");
            st->link_kind = SFTP_MSG_INFO;
            changed = true;
        }
        /* Down: the SFTP handle is about to go (or has gone) with the
         * transport. Keep the folder to come back to. */
        if (st->prev_status == SESSION_CONNECTED && cur != SESSION_CONNECTED) {
            if (rp->fb.source == FB_SOURCE_SFTP && rp->fb.cwd[0])
                snprintf(st->want_remote, sizeof st->want_remote, "%s", rp->fb.cwd);
            fb_detach_sftp(&rp->fb);
            rp->view_dirty = true;
            if (st->drag_pane == SFTP_PANE_REMOTE || st->drop_pane == SFTP_PANE_REMOTE) {
                st->drag_armed = st->drag_live = false;
                st->drop_pane = -1;
            }
            if (st->menu_open && st->menu_pane == SFTP_PANE_REMOTE) st->menu_open = false;
            changed = true;
        }
        /* Up: list where it was, or the account's home. */
        if (cur == SESSION_CONNECTED && st->prev_status != SESSION_CONNECTED) {
            void *sftp = session_sftp_handle(s);
            const char *home = session_initial_cwd(s);
            snprintf(rp->home, sizeof rp->home, "%s", home && home[0] ? home : "/");
            const char *target = st->want_remote[0] ? st->want_remote : rp->home;
            if (sftp) {
                rp->fb.session = s;
                if (fb_navigate_sftp(&rp->fb, sftp, target)) {
                    if (!rp->hist.count) sftp_hist_visit(&rp->hist, target);
                } else {
                    pane_say(rp, SFTP_MSG_ERROR, "The remote folder could not be listed");
                }
                st->was_up = true;
            }
            rp->view_dirty = true;
            changed = true;
        }
        st->prev_status = cur;
    }

    /* Files dropped from the Finder, all of one drop in one transfer. */
    if (st->queue_n > 0 && !app_sftp_busy(app)) {
        if (pane_ready(app, tab, st->queue_to)) {
            app_sftp_transfer_paths(app, st, (const char *const *)st->queue, st->queue_n,
                                    st->queue_to, st->queue_dest);
        } else {
            pane_say(&st->pane[st->queue_to], SFTP_MSG_ERROR, "Not connected to the host");
        }
        queue_clear(st);
        changed = true;
    }

    /* Rows must not move under a hand that is about to act on them. */
    bool hold = st->drag_live || st->menu_open ||
                (app->fb_prompt_active && app->fb_prompt_actor.id == st->id);
    st->pane[0].fb.list_paused = hold;
    st->pane[1].fb.list_paused = hold;

    for (i32 p = 0; p < 2; p++) {
        SftpPane *pp = &st->pane[p];
        if (pane_view_sync(pp)) changed = true;
        if (pp->msg[0] && pp->msg_until > 0.0 && now >= pp->msg_until) {
            pp->msg[0] = '\0';
            pp->msg_kind = SFTP_MSG_NONE;
            changed = true;
        }
    }
    if (!tab->custom_title[0] && strcmp(tab->title, st->name)) {
        snprintf(tab->title, sizeof tab->title, "%s", st->name);
        changed = true;
    }
    return changed;
}

/* ---- drawing helpers ------------------------------------------------------ */

static i32 text_cols(const char *s, usize n) { return (i32)utf8_display_cols((const u8 *)s, n); }

/* `s` (its first `n` bytes) at x,y in at most `cells` cells, ellipsized. */
static void text_fit(Renderer *r, const char *s, usize n, f32 x, f32 y, i32 cells, Color c, f32 cw) {
    if (!s || cells <= 0) return;
    char buf[1024], fit[1024];
    if (n >= sizeof buf) n = sizeof buf - 1;
    memcpy(buf, s, n);
    buf[n] = '\0';
    const char *t = ui_fit_cols(buf, cells, fit, sizeof fit);
    draw_text_ex(r, t, x, y, c, (i32)strlen(t), cw);
}

static Color with_a(Color c, f32 a) { c.a = a; return c; }
static Color scale_rgb(Color c, f32 k, f32 a) { return (Color){ c.r * k, c.g * k, c.b * k, a }; }

/* The kind column: the language's name when the highlighter knows one, the
 * table's word otherwise, folders and links by what they are. */
static void kind_of(const FileEntry *e, char *out, usize cap) {
    if (e->is_dir) { snprintf(out, cap, e->is_symlink ? "Folder link" : "Folder"); return; }
    const char *lang = e->name ? syntax_lang_label(e->name, strlen(e->name)) : NULL;
    if (lang && lang[0]) snprintf(out, cap, "%s", lang);
    else sftp_kind_fallback(e->name, out, cap);
    if (e->is_symlink) {
        usize l = strlen(out);
        snprintf(out + l, cap > l ? cap - l : 0, " link");
    }
}

/* ---- the paint ------------------------------------------------------------ */

typedef struct {
    Renderer   *r;
    const Theme *t;
    f32 dpi, cw, ch, op;
    f32 hx, hy;
    Color fg, dim, faint, accent, sel, well, raised, line, danger, safe, warn;
    /* The theme's ANSI 1-6 as the list may draw them: legible on its surface
     * (ui/sftp_ink.h). Index = Theme.ansi index; 0 and 7+ unused. */
    Color ansi[8];
    Color side[2];        /* this machine's hue, the connected host's */
} Paint;

/* A row's ink: its ls colour, or the text colour. */
static Color paint_ink(const Paint *P, SftpInk k) {
    i32 a = sftp_ink_ansi(k);
    return a > 0 && a < 8 ? P->ansi[a] : P->fg;
}

static Color mix_rgb(Color a, Color b, f32 k) {
    return (Color){ a.r + (b.r - a.r) * k, a.g + (b.g - a.g) * k, a.b + (b.b - a.b) * k,
                    a.a + (b.a - a.a) * k };
}

static void paint_button(Paint *P, AppState *app, SftpRect b, IconKind icon, const char *label,
                         bool chevron, bool hot_sticky) {
    Renderer *r = P->r;
    f32 rad = SFTP_BTN_RADIUS_PT * P->dpi;
    Color fill = with_a(P->fg, hot_sticky ? 0.12f : 0.06f);
    renderer_draw_rrect_simple(r, b.x, b.y, b.w, b.h, fill, rad);
    renderer_flush_rrects(r);
    ui_hover_wash(app, r, b.x, b.y, b.w, b.h, rad, P->fg, 0.08f);
    f32 isz = ui_icon_box_px(SFTP_BTN_ICON_PT, P->dpi);
    f32 x = b.x + SFTP_BTN_PAD_PT * P->dpi;
    Color ink = with_a(P->fg, 0.86f);
    if (icon != ICON_NONE) {
        icon_draw(r, icon, x, b.y + floorf((b.h - isz) * 0.5f), isz, ink);
        x += isz + SFTP_BTN_GAP_PT * P->dpi;
    }
    draw_text_ex(r, label, x, b.y + floorf((b.h - P->ch) * 0.5f), ink, (i32)strlen(label), P->cw);
    if (chevron) {
        f32 csz = ui_icon_box_px(12.0f, P->dpi);
        icon_draw(r, ICON_CHEVRON_DOWN, b.x + b.w - SFTP_BTN_PAD_PT * P->dpi - csz + 2.0f * P->dpi,
                  b.y + floorf((b.h - csz) * 0.5f), csz, with_a(P->fg, 0.70f));
    }
}

static void paint_header(Paint *P, AppState *app, Tab *tab, SftpTab *st, i32 pi, const SftpPaneGeom *g) {
    Renderer *r = P->r;
    SftpPane *p = &st->pane[pi];
    bool focused = st->focus == pi;
    renderer_draw_rect(r, g->header.x, g->header.y, g->header.w, g->header.h, P->raised);
    renderer_draw_rect(r, g->nav.x, g->nav.y, g->nav.w, g->nav.h, P->raised);

    /* The side's tile: this machine in the theme's blue, the host in its
     * green while connected — the link's state in its colour otherwise. The
     * header and the path row take a breath of the same hue. */
    Color tile_ink = P->side[pi];
    if (pi == SFTP_PANE_REMOTE) {
        SessionStatus ss = tab->session ? session_status(tab->session) : SESSION_DISCONNECTED;
        if (ss == SESSION_ERROR || ss == SESSION_DISCONNECTED) tile_ink = P->danger;
        else if (ss != SESSION_CONNECTED) tile_ink = P->warn;
    }
    Color tile_fill = with_a(tile_ink, 0.16f);
    renderer_draw_rect(r, g->header.x, g->header.y, g->header.w, g->header.h + g->nav.h,
                       with_a(tile_ink, 0.018f));   /* linear blending: more blooms */
    renderer_flush_rects(r);
    renderer_draw_rrect_simple(r, g->tile.x, g->tile.y, g->tile.w, g->tile.h, tile_fill, 7.0f * P->dpi);
    renderer_flush_rrects(r);
    f32 tisz = ui_icon_box_px(SFTP_TILE_ICON_PT, P->dpi);
    icon_draw(r, pi == SFTP_PANE_LOCAL ? ICON_LAPTOP : ICON_SERVER,
              g->tile.x + floorf((g->tile.w - tisz) * 0.5f), g->tile.y + floorf((g->tile.h - tisz) * 0.5f),
              tisz, tile_ink);

    /* The name. */
    const char *name = pi == SFTP_PANE_LOCAL ? "Local" : st->label;
    i32 cells = (i32)(g->title.w / P->cw);
    Color name_c = focused ? P->fg : with_a(P->fg, 0.78f);
    text_fit(r, name, strlen(name), g->title.x, g->title.y + floorf((g->title.h - P->ch) * 0.5f),
             cells, name_c, P->cw);

    /* Filter: a button, or the field. */
    bool field = p->filter_open || p->filter_len > 0;
    if (field) {
        bool has_kb = app->te_owner == (const void *)p->filter;
        Color fill = P->well;
        Color bd = has_kb ? P->accent : with_a(P->line, 0.9f);
        f32 rad = SFTP_BTN_RADIUS_PT * P->dpi;
        renderer_draw_rrect_bordered(r, g->filter.x, g->filter.y, g->filter.w, g->filter.h,
                                     fill, bd, (has_kb ? 1.5f : 1.0f) * P->dpi, rad, rad, rad, rad,
                                     0.0f, 0.0f, 0.0f, 0.0f);
        renderer_flush_rrects(r);
        f32 isz = ui_icon_box_px(SFTP_BTN_ICON_PT, P->dpi);
        f32 ix = g->filter.x + 8.0f * P->dpi;
        icon_draw(r, ICON_SEARCH, ix, g->filter.y + floorf((g->filter.h - isz) * 0.5f), isz, P->dim);
        f32 tx = ix + isz + 6.0f * P->dpi;
        f32 right = g->filter_clear.w > 0 ? g->filter_clear.x : g->filter.x + g->filter.w - 6.0f * P->dpi;
        i32 max_cells = (i32)((right - tx) / P->cw);
        f32 ty = g->filter.y + floorf((g->filter.h - P->ch) * 0.5f);
        if (p->filter_len == 0 && !has_kb) {
            draw_text_ex(r, "Filter", tx, ty, P->faint, 6, P->cw);
        } else {
            Color caret = with_a(P->accent, ui_caret_alpha());
            ui_draw_edit_field(r, app, p->filter, p->filter_len, tx, ty, P->cw, P->ch,
                               max_cells < 1 ? 1 : max_cells, P->fg, caret, with_a(P->accent, 0.30f),
                               false, has_kb);
            if (p->filter_len == 0) draw_text_ex(r, "Filter", tx + 2.0f * P->dpi, ty, P->faint, 6, P->cw);
        }
        if (g->filter_clear.w > 0) {
            f32 csz = ui_icon_box_px(12.0f, P->dpi);
            bool hot = ui_hover_hit(g->filter_clear.x, g->filter_clear.y, g->filter_clear.w,
                                    g->filter_clear.h, P->hx, P->hy);
            icon_draw(r, ICON_CLOSE, g->filter_clear.x + floorf((g->filter_clear.w - csz) * 0.5f),
                      g->filter_clear.y + floorf((g->filter_clear.h - csz) * 0.5f), csz,
                      hot ? P->fg : P->dim);
        }
    } else {
        paint_button(P, app, g->filter, ICON_FILTER, "Filter", false, false);
    }
    paint_button(P, app, g->actions, ICON_NONE, "Actions", true,
                 st->menu_open && st->menu_from_button && st->menu_pane == pi);

    /* Back / forward. */
    bool can_b = sftp_hist_can_back(&p->hist), can_f = sftp_hist_can_forward(&p->hist);
    for (i32 k = 0; k < 2; k++) {
        SftpRect b = k ? g->forward : g->back;
        bool can = k ? can_f : can_b;
        f32 chip = ui_icon_box_px(FB_HDR_CHIP_PT, P->dpi);
        bool hot = can && ui_hover_hit(b.x, b.y, b.w, b.h, P->hx, P->hy);
        if (hot) {
            renderer_draw_rrect_simple(r, b.x + (b.w - chip) * 0.5f, b.y + (b.h - chip) * 0.5f,
                                       chip, chip, with_a(P->fg, 0.12f), 7.0f * P->dpi);
            renderer_flush_rrects(r);
        }
        f32 isz = ui_icon_box_px(FB_HDR_ICON_PT, P->dpi);
        icon_draw(r, k ? ICON_CHEVRON_RIGHT : ICON_CHEVRON_LEFT, b.x + floorf((b.w - isz) * 0.5f),
                  b.y + floorf((b.h - isz) * 0.5f), isz,
                  can ? (hot ? P->fg : with_a(P->fg, 0.78f)) : with_a(P->fg, 0.26f));
    }

    /* The breadcrumb. */
    const char *cwd = p->fb.cwd;
    SftpCrumbRow *cr = &p->crumbs;
    if (cwd[0]) sftp_crumb_layout(cr, cwd, g->crumbs, P->dpi, P->cw, text_cols);
    else memset(cr, 0, sizeof *cr);
    f32 cisz = ui_icon_box_px(SFTP_CRUMB_ICON_PT, P->dpi);
    for (i32 i = 0; i < cr->boxes; i++) {
        const SftpCrumbBox *b = &cr->box[i];
        bool leaf = b->crumb == cr->count - 1;
        bool hot = !leaf && ui_hover_hit(b->box.x, b->box.y, b->box.w, b->box.h, P->hx, P->hy);
        if (hot) {
            renderer_draw_rrect_simple(r, b->box.x, b->box.y, b->box.w, b->box.h,
                                       with_a(P->fg, 0.10f), 6.0f * P->dpi);
            renderer_flush_rrects(r);
        }
        f32 ty = b->box.y + floorf((b->box.h - P->ch) * 0.5f);
        if (b->crumb < 0) {
            draw_text_ex(r, "\xe2\x80\xa6", b->text_x, ty, P->dim, 1, P->cw);
        } else {
            const SftpCrumb *c = &cr->crumb[b->crumb];
            Color dir = paint_ink(P, SFTP_INK_DIR);
            icon_draw(r, ICON_FT_FOLDER, b->box.x + SFTP_CRUMB_PAD_PT * P->dpi,
                      b->box.y + floorf((b->box.h - cisz) * 0.5f), cisz,
                      leaf ? dir : with_a(dir, 0.70f));
            Color tc = leaf ? P->fg : (hot ? P->fg : with_a(P->fg, 0.66f));
            if (c->start == 0 && c->len == 1 && cwd[0] == '/')
                draw_text_ex(r, "/", b->text_x, ty, tc, 1, P->cw);
            else
                text_fit(r, cwd + c->start, c->len, b->text_x, ty, b->text_cells, tc, P->cw);
        }
        if (i + 1 < cr->boxes) {
            f32 ssz = ui_icon_box_px(11.0f, P->dpi);
            f32 sx = b->box.x + b->box.w + floorf((SFTP_CRUMB_SEP_PT * P->dpi - ssz) * 0.5f);
            icon_draw(r, ICON_CHEVRON_RIGHT, sx, b->box.y + floorf((b->box.h - ssz) * 0.5f), ssz, P->faint);
        }
    }
    renderer_flush_rects(r);
    renderer_flush_glyphs(r);
}

static const char *const COL_TITLE[SFTP_COL_COUNT] = { "Name", "Date Modified", "Size", "Kind", "Permissions" };

static void paint_columns(Paint *P, SftpPane *p, const SftpPaneGeom *g) {
    Renderer *r = P->r;
    renderer_draw_rect(r, g->cols.x, g->cols.y, g->cols.w, g->cols.h, P->raised);
    Color rule = with_a(P->line, 0.55f);
    f32 line = fmaxf(1.0f, CHROME_LINE_PT * P->dpi);
    renderer_draw_rect(r, g->cols.x, g->cols.y, g->cols.w, line, rule);
    renderer_draw_rect(r, g->cols.x, g->cols.y + g->cols.h - line, g->cols.w, line, rule);
    for (i32 c = 1; c < SFTP_COL_COUNT; c++) {
        if (!g->col_shown[c]) continue;
        renderer_draw_rect(r, g->col[c].x, g->cols.y + g->cols.h * 0.25f, line, g->cols.h * 0.5f, rule);
    }
    renderer_flush_rects(r);
    for (i32 c = 0; c < SFTP_COL_COUNT; c++) {
        if (!g->col_shown[c]) continue;
        SftpRect cr = g->col[c];
        bool active = p->sort == (SftpCol)c;
        bool hot = ui_hover_hit(cr.x, cr.y, cr.w, cr.h, P->hx, P->hy);
        f32 tx = c == SFTP_COL_NAME ? cr.x + SFTP_PAD_PT * P->dpi : cr.x + SFTP_COL_PAD_PT * P->dpi;
        f32 ty = cr.y + floorf((cr.h - P->ch) * 0.5f);
        Color tc = active ? P->fg : hot ? with_a(P->fg, 0.80f) : P->dim;
        i32 room = (i32)((cr.w - (tx - cr.x) - 22.0f * P->dpi) / P->cw);
        text_fit(r, COL_TITLE[c], strlen(COL_TITLE[c]), tx, ty, room, tc, P->cw);
        if (active) {
            i32 used = text_cols(COL_TITLE[c], strlen(COL_TITLE[c]));
            if (used > room) used = room;
            f32 asz = ui_icon_box_px(11.0f, P->dpi);
            icon_draw(r, p->sort_desc ? ICON_CHEVRON_DOWN : ICON_CHEVRON_UP,
                      tx + (f32)used * P->cw + 5.0f * P->dpi, cr.y + floorf((cr.h - asz) * 0.5f), asz,
                      with_a(P->fg, 0.75f));
        }
    }
    renderer_flush_glyphs(r);
}

/* What a pane's list says instead of rows. Decided once a frame, before the
 * paint: the geometry places the button under the lines the message took. */
typedef struct {
    bool             shown;          /* the pane has no rows to show */
    char             title[300];
    Color            color;
    NotificationText msg;            /* the message, wrapped */
    bool             button;         /* Reconnect */
} PaneNotice;

static void pane_notice(const Paint *P, AppState *app, Tab *tab, SftpTab *st, i32 pi, i32 cols,
                        PaneNotice *n) {
    memset(n, 0, sizeof *n);
    SftpPane *p = &st->pane[pi];
    FileBrowser *fb = &p->fb;
    if (pi == SFTP_PANE_REMOTE && !remote_ready(app, tab)) {
        SessionStatus ss = tab->session ? session_status(tab->session) : SESSION_DISCONNECTED;
        bool down = ss == SESSION_ERROR || ss == SESSION_DISCONNECTED;
        /* An automatic attempt waits out its back-off in CONNECTING: it is a
         * reconnect for as long as the policy has one scheduled. */
        snprintf(n->title, sizeof n->title, "%s",
                 st->retry_at > 0.0 ? "Reconnecting\xe2\x80\xa6"
                 : down ? "Not connected"
                 : ss == SESSION_KBI_PENDING ? "Waiting for your answer"
                 : "Connecting\xe2\x80\xa6");
        n->shown = true;
        n->color = down ? P->danger : P->fg;
        n->msg = notification_wrap(st->link_msg, cols, SFTP_NOTICE_LINES);
        n->button = down && st->retry_at <= 0.0;
        return;
    }
    if (p->view_count > 0) return;
    bool listing = pi == SFTP_PANE_LOCAL ? fb_local_listing_active(fb) : fb_sftp_listing_active(fb);
    n->shown = true;
    n->color = P->dim;
    if (listing) snprintf(n->title, sizeof n->title, "Loading\xe2\x80\xa6");
    else if (fb->entry_count == 0) snprintf(n->title, sizeof n->title, "This folder could not be read");
    else if (p->filter_len > 0)
        snprintf(n->title, sizeof n->title, "Nothing here matches \xe2\x80\x9c%s\xe2\x80\x9d", p->filter);
    else snprintf(n->title, sizeof n->title, "This folder is empty");
}

static void paint_centred(Paint *P, const SftpPaneGeom *g, const char *s, f32 y, Color c) {
    char fit[512];
    const char *t = ui_fit_cols(s, g->notice.cols, fit, sizeof fit);
    i32 n = text_cols(t, strlen(t));
    draw_text_ex(P->r, t, g->list.x + floorf((g->list.w - (f32)n * P->cw) * 0.5f), y, c, (i32)strlen(t), P->cw);
}

static void paint_notice(Paint *P, AppState *app, const SftpPaneGeom *g, const PaneNotice *n) {
    Renderer *r = P->r;
    if (n->title[0]) paint_centred(P, g, n->title, g->notice.title_y, n->color);
    for (i32 i = 0; i < n->msg.count; i++)
        paint_centred(P, g, n->msg.lines[i], g->notice.msg_y + (f32)i * g->notice.step, P->dim);
    if (n->button) {
        SftpRect b = g->reconnect;
        f32 rad = 7.0f * P->dpi;
        renderer_draw_rrect_simple(r, b.x, b.y, b.w, b.h, P->accent, rad);
        renderer_flush_rrects(r);
        ui_hover_wash(app, r, b.x, b.y, b.w, b.h, rad, (Color){1, 1, 1, 1}, 0.16f);
        const char *lbl = "Reconnect";
        i32 len = (i32)strlen(lbl);
        Color ink = chrome_legible_on(P->accent);
        draw_text_ex(r, lbl, b.x + floorf((b.w - (f32)len * P->cw) * 0.5f), b.y + floorf((b.h - P->ch) * 0.5f),
                     ink, len, P->cw);
    }
    renderer_flush_glyphs(r);
}

static void paint_rows(Paint *P, AppState *app, SftpTab *st, i32 pi, const SftpPaneGeom *g,
                       const PaneNotice *notice) {
    Renderer *r = P->r;
    SftpPane *p = &st->pane[pi];
    FileBrowser *fb = &p->fb;
    bool focused = st->focus == pi && app->window_focused;
    renderer_draw_rect(r, g->list.x, g->list.y, g->list.w, g->list.h, P->well);
    renderer_flush_rects(r);

    if (notice->shown) {
        paint_notice(P, app, g, notice);
        return;
    }

    f32 rh = g->row_h;
    f32 max = sftp_scroll_max(g, p->view_count);
    if (p->scroll_px > max) p->scroll_px = max;
    if (p->scroll_px < 0.0f) p->scroll_px = 0.0f;
    i32 first = (i32)(p->scroll_px / rh);
    i32 last = first + (i32)(g->list.h / rh) + 2;
    if (last > p->view_count) last = p->view_count;
    f32 sub = p->scroll_px - (f32)first * rh;
    f32 isz = ui_icon_box_px(FB_LIST_ICON_PT, P->dpi);
    f32 rail = g->list.x + SFTP_PAD_PT * P->dpi;
    f32 name_x = rail + isz + FB_LIST_ICON_GAP_PT * P->dpi;
    f32 line = fmaxf(1.0f, CHROME_LINE_PT * P->dpi);
    bool dropping_here = st->drag_live && st->drop_pane == pi;
    i64 now = (i64)time(NULL);

    renderer_push_scissor(r, g->list.x, g->list.y, g->list.w, g->list.h);
    /* Pass 1: row fills. */
    for (i32 row = first; row < last; row++) {
        f32 y = g->list.y + (f32)(row - first) * rh - sub;
        i32 ei = p->view[row];
        bool sel = fb_sel_has(fb, ei);
        bool drop = dropping_here && st->drop_row == row;
        bool hot = !st->drag_live && ui_hover_hit(g->list.x, y, g->list.w, rh, P->hx, P->hy);
        if (drop) renderer_draw_rect(r, g->list.x, y, g->list.w, rh, with_a(P->accent, 0.24f));
        else if (sel) renderer_draw_rect(r, g->list.x, y, g->list.w, rh, with_a(P->sel, focused ? 0.32f : 0.16f));
        else if (hot) renderer_draw_rect(r, g->list.x, y, g->list.w, rh, with_a(P->fg, 0.07f));
        if (row == p->cursor && focused)
            renderer_draw_rect(r, g->list.x, y, fmaxf(2.0f, 2.0f * line), rh, P->accent);
    }
    renderer_flush_rects(r);
    /* Pass 2: icons and text. */
    for (i32 row = first; row < last; row++) {
        f32 y = g->list.y + (f32)(row - first) * rh - sub;
        i32 ei = p->view[row];
        FileEntry *e = &fb->entries[ei];
        bool up = entry_is_up(e);
        bool hidden = e->name && e->name[0] == '.' && !up;
        f32 ty = y + floorf((rh - P->ch) * 0.5f);
        f32 iy = y + floorf((rh - isz) * 0.5f);
        if (up) {
            f32 ink = 0.0f;
            icon_ink_insets(ICON_UP, isz, &ink, NULL);
            icon_draw(r, ICON_UP, rail - ink, iy, isz, P->dim);
        } else {
            Color ic = hidden ? with_a(P->dim, 0.72f) : e->is_dir ? paint_ink(P, SFTP_INK_DIR)
                                                      : scale_rgb(P->fg, 0.66f, 0.95f);
            FbFileIcon ik = fb_file_icon_kind(e->name, e->is_dir);
            fb_file_icon_draw(r, ik, rail - fb_file_icon_ink_left(ik, isz), iy, isz, ic);
        }
        /* The local listing stats lazily (a worker, rows on screen first). */
        if (!e->stat_known && pi == SFTP_PANE_LOCAL) fb_entry_ensure_stat(fb, e);

        /* The name in its ls colour (ui/sftp_ink.h); a hidden one stays dim. */
        SftpInk ink = up ? SFTP_INK_PLAIN
                         : sftp_entry_ink(e->name, e->is_dir, e->is_symlink, e->permissions, e->stat_known);
        Color name_c = up ? P->accent : hidden ? with_a(P->dim, 0.80f) : paint_ink(P, ink);
        f32 name_end = g->col[SFTP_COL_NAME].x + g->col[SFTP_COL_NAME].w - 8.0f * P->dpi;
        const char *nm = up ? ".." : (e->name ? e->name : "");
        text_fit(r, nm, strlen(nm), name_x, ty, (i32)((name_end - name_x) / P->cw), name_c, P->cw);
        if (up) continue;

        char cell[64];
        for (i32 c = 1; c < SFTP_COL_COUNT; c++) {
            if (!g->col_shown[c]) continue;
            cell[0] = '\0';
            switch (c) {
            case SFTP_COL_DATE:
                if (e->stat_known) sftp_date_string(e->mtime, now, cell, sizeof cell);
                break;
            case SFTP_COL_SIZE:
                if (e->is_dir) snprintf(cell, sizeof cell, "--");
                else if (e->stat_known) sftp_size_string(e->size, cell, sizeof cell);
                break;
            case SFTP_COL_KIND:
                kind_of(e, cell, sizeof cell);
                break;
            case SFTP_COL_PERMS:
                if (e->stat_known && e->permissions) sftp_perm_string(e->permissions, e->is_dir, e->is_symlink, cell);
                break;
            }
            if (!cell[0]) continue;
            SftpRect cr = g->col[c];
            f32 tx = cr.x + SFTP_COL_PAD_PT * P->dpi;
            i32 room = (i32)((cr.w - SFTP_COL_PAD_PT * P->dpi - 4.0f * P->dpi) / P->cw);
            if (c == SFTP_COL_PERMS) {
                /* Letter by letter: r yellow, w red, x green, the type d / l. */
                i32 n = (i32)strlen(cell);
                if (n > room) n = room;
                for (i32 k = 0; k < n; k++) {
                    i32 a = sftp_perm_char_ansi(cell[k], k);
                    Color pc = a > 0 && a < 8 ? with_a(P->ansi[a], hidden ? 0.60f : 1.0f) : P->faint;
                    draw_text_ex(r, cell + k, tx + (f32)k * P->cw, ty, pc, 1, P->cw);
                }
                continue;
            }
            /* The kind says it in the row's colour, quieter than the name. */
            Color cc = c == SFTP_COL_KIND && ink != SFTP_INK_PLAIN && !hidden
                     ? mix_rgb(P->dim, paint_ink(P, ink), 0.60f) : P->dim;
            text_fit(r, cell, strlen(cell), tx, ty, room, cc, P->cw);
        }
    }
    renderer_flush_glyphs(r);

    /* A drag aimed at this pane's folder, not at a row: the whole list says so. */
    if (dropping_here && st->drop_row < 0) {
        f32 inset = 3.0f * P->dpi;
        renderer_draw_rrect_bordered(r, g->list.x + inset, g->list.y + inset, g->list.w - 2.0f * inset,
                                     g->list.h - 2.0f * inset, with_a(P->accent, 0.05f), P->accent,
                                     2.0f * P->dpi, 8.0f * P->dpi, 8.0f * P->dpi, 8.0f * P->dpi, 8.0f * P->dpi,
                                     0.0f, 0.0f, 0.0f, 0.0f);
        renderer_flush_rrects(r);
    }

    /* The overlay scrollbar, as the explorer's. */
    f32 content = (f32)p->view_count * rh;
    if (content > g->list.h && g->list.h > 0.0f) {
        bool inside = ui_hover_hit(g->list.x, g->list.y, g->list.w, g->list.h, P->hx, P->hy);
        if (p->scroll_px != p->sb_seen) { p->sb_seen = p->scroll_px; p->sb_active_t = platform_time_sec(); }
        if (inside) p->sb_active_t = platform_time_sec();
        f32 a = ui_autohide_alpha(p->sb_active_t, inside);
        if (a > 0.004f) {
            f32 bw = 4.0f * P->dpi;
            f32 bx = g->list.x + g->list.w - bw - 3.0f * P->dpi;
            f32 th = fmaxf(20.0f * P->dpi, (g->list.h / content) * g->list.h);
            f32 ty = g->list.y + (max > 0.0f ? (p->scroll_px / max) * (g->list.h - th) : 0.0f);
            renderer_draw_rrect_simple(r, bx, ty, bw, th, with_a(P->fg, 0.30f * a), bw * 0.5f);
            renderer_flush_rrects(r);
        }
    }
    renderer_pop_scissor(r);
}

static void paint_footer(Paint *P, AppState *app, Tab *tab, SftpTab *st, i32 pi, const SftpPaneGeom *g) {
    if (g->footer.h <= 0.0f) return;
    Renderer *r = P->r;
    SftpPane *p = &st->pane[pi];
    renderer_draw_rect(r, g->footer.x, g->footer.y, g->footer.w, g->footer.h, P->raised);
    f32 line = fmaxf(1.0f, CHROME_LINE_PT * P->dpi);
    renderer_draw_rect(r, g->footer.x, g->footer.y, g->footer.w, line, with_a(P->line, 0.55f));
    renderer_flush_rects(r);
    f32 ty = g->footer.y + floorf((g->footer.h - P->ch) * 0.5f);
    f32 x0 = g->footer.x + SFTP_PAD_PT * P->dpi;
    char left[160] = "";
    if (pane_ready(app, tab, pi)) {
        i32 items = p->view_count;
        if (items > 0 && entry_is_up(pane_entry(p, 0))) items--;
        u64 bytes = 0;
        i32 sel = pane_selected(p, &bytes);
        if (sel > 0) {
            char sz[32];
            sftp_size_string(bytes, sz, sizeof sz);
            snprintf(left, sizeof left, "%d of %d selected%s%s", (int)sel, (int)items,
                     bytes ? " \xc2\xb7 " : "", bytes ? sz : "");
        } else {
            snprintf(left, sizeof left, "%d item%s", (int)items, items == 1 ? "" : "s");
        }
    }
    i32 lcells = text_cols(left, strlen(left));
    draw_text_ex(r, left, x0, ty, P->dim, (i32)strlen(left), P->cw);
    if (p->msg[0]) {
        Color c = p->msg_kind == SFTP_MSG_ERROR ? P->danger : p->msg_kind == SFTP_MSG_OK ? P->safe : P->dim;
        f32 right = g->footer.x + g->footer.w - SFTP_PAD_PT * P->dpi;
        f32 avail = right - (x0 + (f32)(lcells + 3) * P->cw);
        i32 cells = (i32)(avail / P->cw);
        if (cells > 3) {
            char fit[256];
            const char *t = ui_fit_cols(p->msg, cells, fit, sizeof fit);
            i32 n = text_cols(t, strlen(t));
            draw_text_ex(r, t, right - (f32)n * P->cw, ty, c, (i32)strlen(t), P->cw);
        }
    }
    renderer_flush_glyphs(r);
}

void sftp_tab_render(AppState *app, SftpTab *st, f32 x, f32 y, f32 w, f32 h) {
    if (!app || !st) return;
    Tab *tab = NULL;
    for (i32 i = 0; i < app->tab_count; i++) if (app->tabs[i].sftp == st) { tab = &app->tabs[i]; break; }
    if (!tab) return;
    Renderer *r = &app->renderer;
    const Theme *t = app->config.theme ? app->config.theme : &THEME_Liu;
    Paint P;
    memset(&P, 0, sizeof P);
    P.r = r; P.t = t;
    P.dpi = app->dpi_scale;
    P.cw = 8.0f * P.dpi;
    P.ch = 16.0f * P.dpi;
    P.op = app->config.opacity;
    P.hx = app->window_focused ? app->hover_x : -1.0f;
    P.hy = app->window_focused ? app->hover_y : -1.0f;
    P.fg = t->sidebar_fg; P.fg.a = 1.0f;
    P.dim = scale_rgb(t->sidebar_fg, 0.62f, 0.95f);
    P.faint = scale_rgb(t->sidebar_fg, 0.45f, 0.85f);
    P.accent = theme_ui_accent(t);
    P.sel = t->sidebar_active;
    P.well = t->sidebar_bg; P.well.a *= P.op;
    P.raised = (Color){ fminf(1.0f, t->sidebar_bg.r + 0.02f), fminf(1.0f, t->sidebar_bg.g + 0.02f),
                        fminf(1.0f, t->sidebar_bg.b + 0.02f), t->sidebar_bg.a * P.op };
    P.line = t->border;
    P.danger = status_color_danger(t);
    P.safe = status_color_safe(t);
    P.warn = status_color_warn(t);
    /* The theme's palette, legible as text on the list (t->bg) — and the two
     * sides' hues, legible as a mark on the header. */
    for (i32 i = 1; i <= 6; i++) P.ansi[i] = sftp_ink_on(t->ansi[i], t->bg, SFTP_INK_MIN_CONTRAST);
    P.side[SFTP_PANE_LOCAL]  = sftp_ink_on(t->ansi[4], P.raised, 3.0f);
    P.side[SFTP_PANE_REMOTE] = sftp_ink_on(t->ansi[2], P.raised, 3.0f);

    Color bg = t->bg;
    bg.a = P.op;
    renderer_draw_rect(r, x, y, w, h, bg);
    renderer_flush_rects(r);

    bool fo[2] = { st->pane[0].filter_open || st->pane[0].filter_len > 0,
                   st->pane[1].filter_open || st->pane[1].filter_len > 0 };
    bool ft[2] = { st->pane[0].filter_len > 0, st->pane[1].filter_len > 0 };
    sftp_geom(&st->geom, x, y, w, h, st->ratio, P.dpi, P.cw, P.ch, fo, ft);
    /* Each list's notice, and the stack it is laid out as: the click on
     * Reconnect is tested against the rect this leaves. */
    PaneNotice notice[2];
    for (i32 pi = 0; pi < 2; pi++) {
        SftpPaneGeom *g = &st->geom.p[pi];
        pane_notice(&P, app, tab, st, pi, g->notice.cols, &notice[pi]);
        sftp_pane_notice(g, P.dpi, P.cw, P.ch, notice[pi].msg.count, notice[pi].button);
    }
    st->geom_valid = true;

    renderer_set_ui_scale(r, P.cw, P.ch);
    for (i32 pi = 0; pi < 2; pi++) {
        const SftpPaneGeom *g = &st->geom.p[pi];
        if (g->pane.w <= 0.0f) continue;
        renderer_push_scissor(r, g->pane.x, g->pane.y, g->pane.w, g->pane.h);
        paint_header(&P, app, tab, st, pi, g);
        paint_columns(&P, &st->pane[pi], g);
        paint_rows(&P, app, st, pi, g, &notice[pi]);
        renderer_push_scissor(r, g->pane.x, g->pane.y, g->pane.w, g->pane.h);
        paint_footer(&P, app, tab, st, pi, g);
        renderer_pop_scissor(r);
    }
    /* The divider. */
    Color div = t->border;
    div.a = st->divider_drag ? 1.0f : 0.9f;
    bool grab_hot = ui_hover_hit(st->geom.grab.x, st->geom.grab.y, st->geom.grab.w, st->geom.grab.h, P.hx, P.hy);
    if (grab_hot || st->divider_drag) div = with_a(P.accent, 0.8f);
    renderer_draw_rect(r, st->geom.divider.x, st->geom.divider.y, st->geom.divider.w, st->geom.divider.h, div);
    renderer_flush_rects(r);
    renderer_reset_ui_scale(r);
}

/* ---- the menu -------------------------------------------------------------- */

static void menu_target(AppState *app, Tab *tab, SftpTab *st, i32 pi, SftpMenuTarget *t) {
    memset(t, 0, sizeof *t);
    SftpPane *p = &st->pane[pi];
    t->remote = pi == SFTP_PANE_REMOTE;
    t->connected = pane_ready(app, tab, pi);
    t->other_ready = pane_ready(app, tab, 1 - pi);
    t->selection = pane_selected(p, NULL);
    t->has_entry = t->selection > 0;
    if (t->selection == 1) {
        for (i32 r = 0; r < p->view_count; r++) {
            if (fb_sel_has(&p->fb, p->view[r])) { t->is_dir = p->fb.entries[p->view[r]].is_dir; break; }
        }
    }
    t->clip_has = app->file_clipboard.has;
    t->show_hidden = p->show_hidden;
    t->busy = app_sftp_busy(app);
    t->can_trash = !t->remote && app_fb_trash_available(app);
}

typedef struct {
    SftpMenuRows rows;
    CtxMenuBox   box;
    CtxMenuRow   paint[SFTP_ACT_COUNT];
    char         labels[SFTP_ACT_COUNT][96];
    char         keys[SFTP_ACT_COUNT][32];
} MenuLayout;

static void menu_layout(AppState *app, Tab *tab, SftpTab *st, MenuLayout *L) {
    memset(L, 0, sizeof *L);
    SftpMenuTarget t;
    menu_target(app, tab, st, st->menu_pane, &t);
    sftp_menu_rows(&t, &L->rows);
    static const IconKind icon[SFTP_ACT_COUNT] = {
        [SFTP_ACT_OPEN] = ICON_FT_FOLDER, [SFTP_ACT_SEND] = ICON_UPLOAD, [SFTP_ACT_COPY] = ICON_COPY,
        [SFTP_ACT_CUT] = ICON_SCISSORS, [SFTP_ACT_PASTE] = ICON_CLIPBOARD,
        [SFTP_ACT_NEW_FOLDER] = ICON_FOLDER_PLUS, [SFTP_ACT_RENAME] = ICON_PENCIL,
        [SFTP_ACT_REFRESH] = ICON_REFRESH, [SFTP_ACT_HIDDEN] = ICON_EYE, [SFTP_ACT_HOME] = ICON_HOME,
        [SFTP_ACT_DELETE] = ICON_TRASH,
    };
    for (i32 i = 0; i < L->rows.count; i++) {
        SftpMenuAct a = L->rows.acts[i];
        L->paint[i].icon = icon[a];
        if (a == SFTP_ACT_SEND && t.remote) L->paint[i].icon = ICON_DOWNLOAD;
        if (a == SFTP_ACT_HIDDEN && t.show_hidden) L->paint[i].icon = ICON_EYE_OFF;
        L->paint[i].label = sftp_menu_label(a, &t, st->label, L->labels[i], sizeof L->labels[i]);
        L->paint[i].keys = sftp_menu_shortcut(a, UI_MOD_APP, L->keys[i], sizeof L->keys[i]);
        L->paint[i].enabled = L->rows.enabled[i];
        L->paint[i].danger = sftp_menu_destructive(a);
    }
    f32 dpi = app->dpi_scale;
    f32 w = ctx_menu_measure(L->paint, L->rows.count, dpi);
    ctx_menu_place(&L->box, L->rows.count, L->rows.sep_after, st->menu_x, st->menu_y, w,
                   (f32)app->fb_width, (f32)app->fb_height, dpi);
}

bool sftp_tab_menu_open(const AppState *app) {
    const SftpTab *st = active_sftp(app);
    return st && st->menu_open;
}

static void menu_open_at(AppState *app, SftpTab *st, i32 pi, f32 x, f32 y, bool from_button) {
    st->menu_open = true;
    st->menu_pane = pi;
    st->menu_sel = -1;
    st->menu_by_key = false;
    st->menu_x = x;
    st->menu_y = y;
    st->menu_from_button = from_button;
    st->focus = pi;
    (void)app;
}

/* The row of the selection to name in an action that takes one (rename). */
static i32 pane_first_selected_entry(const SftpPane *p) {
    if (p->cursor >= 0) {
        const FileEntry *e = pane_entry(p, p->cursor);
        if (e && fb_sel_has(&p->fb, p->view[p->cursor])) return p->view[p->cursor];
    }
    for (i32 r = 0; r < p->view_count; r++) if (fb_sel_has(&p->fb, p->view[r])) return p->view[r];
    return -1;
}

static void do_send(AppState *app, Tab *tab, SftpTab *st, i32 from) {
    i32 to = 1 - from;
    if (!pane_ready(app, tab, to)) {
        pane_say(&st->pane[from], SFTP_MSG_ERROR, to == SFTP_PANE_REMOTE ? "Not connected to the host"
                                                                          : "Nothing to send to");
        return;
    }
    SftpPane *p = &st->pane[from];
    i32 entry = pane_first_selected_entry(p);
    if (entry < 0) return;
    app_sftp_transfer(app, st, from, entry, to, st->pane[to].fb.cwd, false);
}

static void open_row(AppState *app, Tab *tab, SftpTab *st, i32 pi, i32 row) {
    SftpPane *p = &st->pane[pi];
    const FileEntry *e = pane_entry(p, row);
    if (!e) return;
    if (entry_is_up(e)) { pane_up(app, tab, pi); return; }
    if (e->is_dir) {
        char path[FB_MAX_PATH * 2];
        fb_entry_path(&p->fb, e, path, sizeof path);
        if (path[0]) pane_go(app, tab, pi, path, true);
        return;
    }
    /* A file: across to the other side, as in every two-pane transfer tool. */
    pane_select_only(p, row);
    p->cursor = row;
    do_send(app, tab, st, pi);
}

static void menu_act(AppState *app, Tab *tab, SftpTab *st, SftpMenuAct a) {
    i32 pi = st->menu_pane;
    SftpPane *p = &st->pane[pi];
    st->menu_open = false;
    i32 entry = pane_first_selected_entry(p);
    switch (a) {
    case SFTP_ACT_OPEN:
        if (entry >= 0) { i32 row = pane_row_of(p, entry); if (row >= 0) open_row(app, tab, st, pi, row); }
        break;
    case SFTP_ACT_SEND:       do_send(app, tab, st, pi); break;
    case SFTP_ACT_COPY:       app_sftp_fb_action(app, st, pi, FB_ACT_COPY, entry); break;
    case SFTP_ACT_CUT:        app_sftp_fb_action(app, st, pi, FB_ACT_CUT, entry); break;
    case SFTP_ACT_PASTE:      app_sftp_fb_action(app, st, pi, FB_ACT_PASTE, -1); break;
    case SFTP_ACT_NEW_FOLDER: app_sftp_fb_action(app, st, pi, FB_ACT_NEW_FOLDER, -1); break;
    case SFTP_ACT_RENAME:     if (entry >= 0) app_sftp_fb_action(app, st, pi, FB_ACT_RENAME, entry); break;
    case SFTP_ACT_DELETE:     if (entry >= 0) app_sftp_fb_action(app, st, pi, FB_ACT_DELETE, entry); break;
    case SFTP_ACT_REFRESH:    if (p->fb.cwd[0]) fb_refresh_keep(&p->fb, NULL); p->view_dirty = true; break;
    case SFTP_ACT_HIDDEN:     p->show_hidden = !p->show_hidden; p->view_dirty = true; break;
    case SFTP_ACT_HOME:       if (p->home[0]) pane_go(app, tab, pi, p->home, true); break;
    default: break;
    }
}

void sftp_tab_render_overlay(AppState *app) {
    SftpTab *st = active_sftp(app);
    if (!st) return;
    Tab *tab = &app->tabs[app->active_tab];
    Renderer *r = &app->renderer;
    const Theme *t = app->config.theme ? app->config.theme : &THEME_Liu;
    f32 dpi = app->dpi_scale, cw = 8.0f * dpi, ch = 16.0f * dpi;
    renderer_set_ui_scale(r, cw, ch);
    if (st->menu_open) {
        MenuLayout L;
        menu_layout(app, tab, st, &L);
        /* The pointer owns the highlight inside the menu (ctx_menu_pointer). */
        bool inside = ctx_menu_contains(&L.box, app->hover_x, app->hover_y);
        i32 row = ctx_menu_row_at(&L.box, app->hover_x, app->hover_y);
        if (row >= 0 && !L.rows.enabled[row]) row = -1;
        st->menu_sel = ctx_menu_pointer(st->menu_sel, inside, row, &st->menu_by_key);
        ui_hover_hit(L.box.x, L.box.y, L.box.w, L.box.h, app->hover_x, app->hover_y);
        for (i32 i = 0; i < L.box.count; i++)
            ui_hover_hit(L.box.x, L.box.row_y[i], L.box.w, L.box.row_h, app->hover_x, app->hover_y);
        ctx_menu_draw_panel(r, t, L.box.x, L.box.y, L.box.w, L.box.h, dpi);
        ctx_menu_draw_rows(r, t, &L.box, L.paint, st->menu_sel, dpi);
    }
    if (st->drag_live) {
        /* The ghost: what is being carried, beside the pointer. */
        char label[300];
        if (st->drag_count > 1) snprintf(label, sizeof label, "%d items", (int)st->drag_count);
        else snprintf(label, sizeof label, "%s", st->drag_name);
        char fit[300];
        const char *lt = ui_fit_cols(label, 28, fit, sizeof fit);
        i32 n = text_cols(lt, strlen(lt));
        f32 isz = ui_icon_box_px(FB_LIST_ICON_PT, dpi);
        f32 pad = 10.0f * dpi;
        f32 gw = pad + isz + 8.0f * dpi + (f32)n * cw + pad;
        f32 gh = 30.0f * dpi;
        f32 gx = app->hover_x + 14.0f * dpi, gy = app->hover_y + 10.0f * dpi;
        Color body = t->tab_active_bg; body.a = 0.96f;
        Color bd = theme_ui_accent(t); bd.a = 0.85f;
        draw_panel_chrome(r, gx, gy, gw, gh, body, bd, 8.0f * dpi, dpi, 12.0f * dpi, 0.30f);
        renderer_flush_rrects(r);
        IconKind ik = st->drop_pane < 0 ? (st->drag_first_dir ? ICON_FT_FOLDER : ICON_FT_DOC)
                    : st->drop_pane == SFTP_PANE_REMOTE && st->drag_pane == SFTP_PANE_LOCAL ? ICON_UPLOAD
                    : st->drop_pane == SFTP_PANE_LOCAL && st->drag_pane == SFTP_PANE_REMOTE ? ICON_DOWNLOAD
                    : st->drag_first_dir ? ICON_FT_FOLDER : ICON_FT_DOC;
        icon_draw(r, ik, gx + pad, gy + floorf((gh - isz) * 0.5f), isz, theme_ui_accent(t));
        draw_text_ex(r, lt, gx + pad + isz + 8.0f * dpi, gy + floorf((gh - ch) * 0.5f), t->fg,
                     (i32)strlen(lt), cw);
        if (st->drag_count > 1) {
            f32 bsz = 18.0f * dpi;
            renderer_draw_rrect_simple(r, gx + gw - bsz * 0.5f, gy - bsz * 0.4f, bsz, bsz,
                                       theme_ui_accent(t), bsz * 0.5f);
            renderer_flush_rrects(r);
        }
        renderer_flush_glyphs(r);
    }
    renderer_reset_ui_scale(r);
}

/* ---- the pointer ------------------------------------------------------------ */

static SftpHit hit_at(SftpTab *st, f32 x, f32 y, bool reconnect_shown) {
    f32 sc[2] = { st->pane[0].scroll_px, st->pane[1].scroll_px };
    i32 rows[2] = { st->pane[0].view_count, st->pane[1].view_count };
    SftpCrumbRow crumbs[2] = { st->pane[0].crumbs, st->pane[1].crumbs };
    return sftp_hit(&st->geom, x, y, sc, rows, crumbs, reconnect_shown);
}

static bool reconnect_shown(const Tab *tab, const SftpTab *st) {
    SessionStatus ss = tab->session ? session_status(tab->session) : SESSION_DISCONNECTED;
    return (ss == SESSION_ERROR || ss == SESSION_DISCONNECTED) && st->retry_at <= 0.0;
}

static void filter_focus(AppState *app, SftpTab *st, i32 pi) {
    SftpPane *p = &st->pane[pi];
    p->filter_open = true;
    st->focus = pi;
    TextEdit e = app_edit(app, p->filter, &p->filter_len, SFTP_FILTER_CAP);
    (void)e;
}

static void filter_blur(AppState *app, SftpTab *st) {
    for (i32 i = 0; i < 2; i++) {
        SftpPane *p = &st->pane[i];
        if (app->te_owner == (const void *)p->filter) app->te_owner = NULL;
        if (p->filter_len == 0) p->filter_open = false;
    }
}

static void reconnect(Tab *tab, SftpTab *st) {
    Session *s = tab->session;
    if (!s) return;
    if (!session_link_retry_now(s) && !session_reconnect(s)) return;
    st->reconnects++;
    snprintf(st->link_msg, sizeof st->link_msg, "Connecting to %s\xe2\x80\xa6", st->label);
    st->link_kind = SFTP_MSG_INFO;
}

bool sftp_tab_mouse_down(AppState *app, f32 x, f32 y, i32 button, u32 mods) {
    SftpTab *st = active_sftp(app);
    if (!st || !st->geom_valid || app_modal_open(app)) return false;
    Tab *tab = &app->tabs[app->active_tab];

    if (st->menu_open) {
        MenuLayout L;
        menu_layout(app, tab, st, &L);
        bool inside = ctx_menu_contains(&L.box, x, y);
        i32 row = ctx_menu_row_at(&L.box, x, y);
        if (inside && (row < 0 || !L.rows.enabled[row])) return true;    /* a click ON the menu */
        st->menu_open = false;
        if (inside) menu_act(app, tab, st, L.rows.acts[row]);
        return true;
    }

    SftpHit h = hit_at(st, x, y, reconnect_shown(tab, st));
    if (h.kind == SFTP_HIT_NONE) return false;
    if (h.kind != SFTP_HIT_FILTER && h.kind != SFTP_HIT_FILTER_CLEAR) filter_blur(app, st);
    if (h.pane >= 0) st->focus = h.pane;
    SftpPane *p = h.pane >= 0 ? &st->pane[h.pane] : NULL;

    if (button == 1) {                                   /* right: the menu */
        if (!p) return true;
        if (h.kind == SFTP_HIT_ROW) {
            i32 ei = p->view[h.index];
            if (!fb_sel_has(&p->fb, ei)) pane_select_only(p, h.index);
            p->cursor = h.index;
            p->anchor = h.index;
        } else if (h.kind == SFTP_HIT_LIST) {
            fb_sel_clear(&p->fb);
            p->cursor = -1;
        }
        menu_open_at(app, st, h.pane, x, y, false);
        return true;
    }

    switch (h.kind) {
    case SFTP_HIT_DIVIDER:
        st->divider_drag = true;
        return true;
    case SFTP_HIT_FILTER:
        filter_focus(app, st, h.pane);
        return true;
    case SFTP_HIT_FILTER_CLEAR:
        p->filter[0] = '\0';
        p->filter_len = 0;
        p->view_dirty = true;
        filter_focus(app, st, h.pane);
        return true;
    case SFTP_HIT_ACTIONS: {
        const SftpPaneGeom *g = &st->geom.p[h.pane];
        menu_open_at(app, st, h.pane, g->actions.x + g->actions.w, g->actions.y + g->actions.h + 4.0f * app->dpi_scale, true);
        /* Right-aligned under the button: place it, then pull it left by its width. */
        MenuLayout L;
        menu_layout(app, tab, st, &L);
        st->menu_x = g->actions.x + g->actions.w - L.box.w;
        return true;
    }
    case SFTP_HIT_BACK: {
        if (h.pane == SFTP_PANE_REMOTE && !remote_ready(app, tab)) return true;
        SftpHistory hist_was = p->hist;
        const char *to = sftp_hist_back(&p->hist);
        if (to) { char path[SFTP_HIST_PATH]; snprintf(path, sizeof path, "%s", to);
                  if (!pane_go(app, tab, h.pane, path, false)) p->hist = hist_was; }
        return true;
    }
    case SFTP_HIT_FORWARD: {
        if (h.pane == SFTP_PANE_REMOTE && !remote_ready(app, tab)) return true;
        SftpHistory hist_was = p->hist;
        const char *to = sftp_hist_forward(&p->hist);
        if (to) { char path[SFTP_HIST_PATH]; snprintf(path, sizeof path, "%s", to);
                  if (!pane_go(app, tab, h.pane, path, false)) p->hist = hist_was; }
        return true;
    }
    case SFTP_HIT_CRUMB: {
        if (h.index < 0 || h.index >= p->crumbs.count) return true;
        if (h.index == p->crumbs.count - 1) return true;          /* the folder it is in */
        const SftpCrumb *c = &p->crumbs.crumb[h.index];
        char path[FB_MAX_PATH];
        usize n = c->end < sizeof path ? c->end : sizeof path - 1;
        memcpy(path, p->fb.cwd, n);
        path[n] = '\0';
        if (!path[0]) snprintf(path, sizeof path, "/");
        pane_go(app, tab, h.pane, path, true);
        return true;
    }
    case SFTP_HIT_COLUMN:
        if (p->sort == (SftpCol)h.index) p->sort_desc = !p->sort_desc;
        else { p->sort = (SftpCol)h.index; p->sort_desc = (h.index == SFTP_COL_DATE || h.index == SFTP_COL_SIZE); }
        p->view_dirty = true;
        return true;
    case SFTP_HIT_RECONNECT:
        reconnect(tab, st);
        return true;
    case SFTP_HIT_LIST:
        fb_sel_clear(&p->fb);
        p->cursor = -1;
        return true;
    case SFTP_HIT_ROW: {
        i32 row = h.index;
        i32 ei = p->view[row];
        const FileEntry *e = pane_entry(p, row);
        f64 now = platform_time_sec();
        bool dbl = st->click_pane == h.pane && st->click_row == row && now - st->click_t < SFTP_DOUBLE_CLICK_S;
        st->click_t = dbl ? 0.0 : now;
        st->click_pane = h.pane;
        st->click_row = row;
        if (dbl && !(mods & (MOD_SHIFT | MOD_APP))) { open_row(app, tab, st, h.pane, row); return true; }
        if (mods & MOD_APP) {
            if (!entry_is_up(e)) fb_sel_toggle(&p->fb, ei);
            p->anchor = row;
        } else if (mods & MOD_SHIFT) {
            pane_select_range(p, p->anchor >= 0 ? p->anchor : row, row);
        } else {
            if (!fb_sel_has(&p->fb, ei)) pane_select_only(p, row);
            p->anchor = row;
        }
        p->cursor = row;
        /* Armed for a drag; a release without one narrows a multi-selection
         * to this row (sftp_tab_mouse_up). */
        if (!entry_is_up(e) && fb_sel_has(&p->fb, ei)) {
            st->drag_armed = true;
            st->drag_live = false;
            st->drag_pane = h.pane;
            st->drag_x0 = x;
            st->drag_y0 = y;
        }
        return true;
    }
    default:
        return true;
    }
}

/* Where a drag at (x, y) would land: a pane, and a folder row of it (-1 = its
 * own folder). Nothing in the pane it came from unless it is a folder that is
 * not being carried. */
static void drop_target(AppState *app, Tab *tab, SftpTab *st, f32 x, f32 y) {
    st->drop_pane = -1;
    st->drop_row = -1;
    SftpHit h = hit_at(st, x, y, false);
    if (h.pane < 0) return;
    if (!pane_ready(app, tab, h.pane)) return;
    SftpPane *p = &st->pane[h.pane];
    if (h.kind == SFTP_HIT_ROW) {
        const FileEntry *e = pane_entry(p, h.index);
        if (e && e->is_dir && !entry_is_up(e) && !(h.pane == st->drag_pane && fb_sel_has(&p->fb, p->view[h.index]))) {
            st->drop_pane = h.pane;
            st->drop_row = h.index;
            return;
        }
    }
    if (h.pane != st->drag_pane) st->drop_pane = h.pane;
}

i32 sftp_tab_cursor(AppState *app, f32 x, f32 y) {
    SftpTab *st = active_sftp(app);
    if (!st || !st->geom_valid) return CURSOR_DEFAULT;
    if (st->divider_drag) return CURSOR_RESIZE_H;
    SftpHit h = hit_at(st, x, y, false);
    if (h.kind == SFTP_HIT_DIVIDER) return CURSOR_RESIZE_H;
    if (h.kind == SFTP_HIT_FILTER && h.pane >= 0 &&
        (st->pane[h.pane].filter_open || st->pane[h.pane].filter_len > 0)) return CURSOR_TEXT;
    return CURSOR_DEFAULT;
}

bool sftp_tab_mouse_move(AppState *app, f32 x, f32 y) {
    SftpTab *st = active_sftp(app);
    if (!st || !st->geom_valid) return false;
    Tab *tab = &app->tabs[app->active_tab];
    if (st->divider_drag) {
        f32 tx = st->geom.p[0].pane.x;
        f32 tw = st->geom.p[0].pane.w + st->geom.divider.w + st->geom.p[1].pane.w;
        if (tw > 0.0f) st->ratio = sftp_ratio_clamp((x - tx) / tw, tw, app->dpi_scale);
        return true;
    }
    if (st->drag_armed && !st->drag_live) {
        f32 dx = x - st->drag_x0, dy = y - st->drag_y0;
        f32 th = SFTP_DRAG_START_PT * app->dpi_scale;
        if (dx * dx + dy * dy >= th * th) {
            SftpPane *p = &st->pane[st->drag_pane];
            st->drag_live = true;
            st->drag_count = pane_selected(p, NULL);
            st->drag_name[0] = '\0';
            st->drag_first_dir = false;
            i32 ei = pane_first_selected_entry(p);
            if (ei >= 0) {
                snprintf(st->drag_name, sizeof st->drag_name, "%s", p->fb.entries[ei].name);
                st->drag_first_dir = p->fb.entries[ei].is_dir;
            }
        }
    }
    if (st->drag_live) {
        i32 dp = st->drop_pane, dr = st->drop_row;
        drop_target(app, tab, st, x, y);
        (void)dp; (void)dr;
        return true;                           /* the ghost follows the pointer */
    }
    return false;
}

bool sftp_tab_mouse_up(AppState *app, f32 x, f32 y, i32 button, u32 mods) {
    (void)button;
    SftpTab *st = active_sftp(app);
    if (!st) return false;
    Tab *tab = &app->tabs[app->active_tab];
    if (st->divider_drag) { st->divider_drag = false; return true; }
    if (st->drag_live) {
        drop_target(app, tab, st, x, y);
        i32 from = st->drag_pane, to = st->drop_pane, row = st->drop_row;
        st->drag_armed = st->drag_live = false;
        st->drop_pane = -1;
        st->drop_row = -1;
        if (to < 0) return true;
        SftpPane *src = &st->pane[from];
        i32 entry = pane_first_selected_entry(src);
        if (entry < 0) return true;
        if (to == from) {
            const FileEntry *dir = pane_entry(&st->pane[to], row);
            if (dir) app_sftp_move_into(app, st, from, st->pane[to].view[row]);
            return true;
        }
        char dest[FB_MAX_PATH * 2];
        if (row >= 0) fb_entry_path(&st->pane[to].fb, pane_entry(&st->pane[to], row), dest, sizeof dest);
        else snprintf(dest, sizeof dest, "%s", st->pane[to].fb.cwd);
        st->focus = to;
        app_sftp_transfer(app, st, from, entry, to, dest, false);
        return true;
    }
    if (st->drag_armed) {
        st->drag_armed = false;
        /* A plain click on a row inside a selection of several: just that row. */
        if (!(mods & (MOD_SHIFT | MOD_APP))) {
            SftpHit h = hit_at(st, x, y, false);
            if (h.kind == SFTP_HIT_ROW && h.pane == st->drag_pane) {
                SftpPane *p = &st->pane[h.pane];
                if (pane_selected(p, NULL) > 1) pane_select_only(p, h.index);
            }
        }
        return true;
    }
    return false;
}

bool sftp_tab_scroll(AppState *app, f32 x, f32 y, f32 dy, bool precise) {
    SftpTab *st = active_sftp(app);
    if (!st || !st->geom_valid) return false;
    if (st->menu_open) return true;
    SftpHit h = hit_at(st, x, y, false);
    if (h.pane < 0) return false;
    SftpPane *p = &st->pane[h.pane];
    const SftpPaneGeom *g = &st->geom.p[h.pane];
    f32 step = precise ? dy : dy * g->row_h;
    f32 before = p->scroll_px;
    p->scroll_px -= step;
    f32 max = sftp_scroll_max(g, p->view_count);
    if (p->scroll_px < 0.0f) p->scroll_px = 0.0f;
    if (p->scroll_px > max) p->scroll_px = max;
    return p->scroll_px != before;
}

/* ---- the keyboard ------------------------------------------------------------ */

static void cursor_to(SftpTab *st, i32 pi, i32 row, bool extend) {
    SftpPane *p = &st->pane[pi];
    if (p->view_count <= 0) return;
    if (row < 0) row = 0;
    if (row >= p->view_count) row = p->view_count - 1;
    if (extend) {
        if (p->anchor < 0) p->anchor = p->cursor >= 0 ? p->cursor : row;
        pane_select_range(p, p->anchor, row);
    } else {
        pane_select_only(p, row);
        p->anchor = row;
    }
    p->cursor = row;
    if (st->geom_valid)
        p->scroll_px = sftp_scroll_to_row(&st->geom.p[pi], p->scroll_px, row, p->view_count);
}

bool sftp_tab_key(AppState *app, i32 key, u32 mods) {
    SftpTab *st = active_sftp(app);
    if (!st || app_modal_open(app)) return false;
    Tab *tab = &app->tabs[app->active_tab];
    i32 pi = st->focus == SFTP_PANE_LOCAL ? SFTP_PANE_LOCAL : SFTP_PANE_REMOTE;
    SftpPane *p = &st->pane[pi];
    u32 m = mods & (MOD_SHIFT | MOD_CTRL | MOD_ALT | MOD_SUPER);
    bool cmd = (m & MOD_APP) != 0;
    bool shift = (m & MOD_SHIFT) != 0;

    /* The menu has the arrows and Enter while it is open. */
    if (st->menu_open) {
        MenuLayout L;
        menu_layout(app, tab, st, &L);
        if (key == KEY_ESCAPE) { st->menu_open = false; return true; }
        if (key == KEY_UP || key == KEY_DOWN) {
            st->menu_sel = ctx_menu_step(L.rows.count, L.rows.enabled, st->menu_sel, key == KEY_DOWN ? 1 : -1);
            st->menu_by_key = true;
            return true;
        }
        if (key == KEY_ENTER) {
            if (st->menu_sel >= 0 && st->menu_sel < L.rows.count && L.rows.enabled[st->menu_sel])
                menu_act(app, tab, st, L.rows.acts[st->menu_sel]);
            else st->menu_open = false;
            return true;
        }
        return true;
    }

    /* The filter field has the keyboard while it is the shared editor's owner. */
    if (app->te_owner == (const void *)p->filter) {
        if (key == KEY_ESCAPE) {
            p->filter[0] = '\0';
            p->filter_len = 0;
            p->filter_open = false;
            p->view_dirty = true;
            app->te_owner = NULL;
            return true;
        }
        if (key == KEY_ENTER || key == KEY_DOWN || key == KEY_TAB) {
            filter_blur(app, st);
            pane_view_sync(p);
            if (p->view_count > 0) cursor_to(st, pi, (p->view_count > 1 && entry_is_up(pane_entry(p, 0))) ? 1 : 0, false);
            return true;
        }
        TextEdit e = app_edit(app, p->filter, &p->filter_len, SFTP_FILTER_CAP);
        i32 before = p->filter_len;
        char was[SFTP_FILTER_CAP];
        memcpy(was, p->filter, sizeof was);
        bool used = textedit_key(&e, &app->te_undo, key, mods);
        app_edit_done(app, p->filter_len);
        if (p->filter_len != before || memcmp(was, p->filter, sizeof was)) p->view_dirty = true;
        /* Keys the field does not use still belong to it — never to a list
         * behind it the user is not looking at — except the app's chords. */
        return used || !cmd;
    }

    if (key == KEY_ESCAPE && !m) {
        if (pane_selected(p, NULL) > 0) { fb_sel_clear(&p->fb); return true; }
        if (p->filter_len > 0) { p->filter[0] = '\0'; p->filter_len = 0; p->filter_open = false; p->view_dirty = true; return true; }
        return false;
    }
    if (key == KEY_TAB && !(m & ~MOD_SHIFT)) { st->focus = 1 - st->focus; return true; }

    bool ready = pane_ready(app, tab, pi);
    i32 page = st->geom_valid && st->geom.p[pi].row_h > 0.0f
             ? (i32)(st->geom.p[pi].list.h / st->geom.p[pi].row_h) - 1 : 10;
    if (page < 1) page = 1;
    if (!cmd && !(m & (MOD_CTRL | MOD_ALT))) {
        switch (key) {
        case KEY_UP:        cursor_to(st, pi, p->cursor < 0 ? p->view_count - 1 : p->cursor - 1, shift); return true;
        case KEY_DOWN:      cursor_to(st, pi, p->cursor < 0 ? 0 : p->cursor + 1, shift); return true;
        case KEY_HOME:      cursor_to(st, pi, 0, shift); return true;
        case KEY_END:       cursor_to(st, pi, p->view_count - 1, shift); return true;
        case KEY_PAGE_UP:   cursor_to(st, pi, (p->cursor < 0 ? 0 : p->cursor) - page, shift); return true;
        case KEY_PAGE_DOWN: cursor_to(st, pi, (p->cursor < 0 ? 0 : p->cursor) + page, shift); return true;
        case KEY_ENTER:
            if (ready && p->cursor >= 0) open_row(app, tab, st, pi, p->cursor);
            return true;
        case KEY_BACKSPACE:
            if (ready) pane_up(app, tab, pi);
            return true;
        case KEY_F2: {
            i32 e = pane_first_selected_entry(p);
            if (ready && e >= 0) app_sftp_fb_action(app, st, pi, FB_ACT_RENAME, e);
            return true;
        }
        case KEY_DELETE: {
            i32 e = pane_first_selected_entry(p);
            if (ready && e >= 0) app_sftp_fb_action(app, st, pi, FB_ACT_DELETE, e);
            return true;
        }
        default: break;
        }
        return false;
    }
    if (cmd && !(m & (MOD_CTRL | MOD_ALT))) {
        switch (key) {
        case KEY_UP:     if (ready) pane_up(app, tab, pi); return true;
        case KEY_DOWN:   if (ready && p->cursor >= 0) open_row(app, tab, st, pi, p->cursor); return true;
        case KEY_LEFT_BRACKET:
        case KEY_RIGHT_BRACKET: {
            if (!ready || shift) return false;          /* Cmd+Shift+[ ] are the tab keys */
            SftpHistory was = p->hist;
            const char *to = key == KEY_LEFT_BRACKET ? sftp_hist_back(&p->hist) : sftp_hist_forward(&p->hist);
            if (to) { char path[SFTP_HIST_PATH]; snprintf(path, sizeof path, "%s", to);
                      if (!pane_go(app, tab, pi, path, false)) p->hist = was; }
            return true;
        }
        case KEY_ENTER:  if (ready) do_send(app, tab, st, pi); return true;
        case KEY_A:
            if (shift) return false;
            fb_sel_clear(&p->fb);
            for (i32 r = 0; r < p->view_count; r++)
                if (!entry_is_up(pane_entry(p, r))) fb_sel_set(&p->fb, p->view[r], true);
            return true;
        case KEY_C: if (shift) return false; if (ready) app_sftp_fb_action(app, st, pi, FB_ACT_COPY, pane_first_selected_entry(p)); return true;
        case KEY_X: if (shift) return false; if (ready) app_sftp_fb_action(app, st, pi, FB_ACT_CUT, pane_first_selected_entry(p)); return true;
        case KEY_V: if (shift) return false; if (ready) app_sftp_fb_action(app, st, pi, FB_ACT_PASTE, -1); return true;
        case KEY_N: if (!shift) return false; if (ready) app_sftp_fb_action(app, st, pi, FB_ACT_NEW_FOLDER, -1); return true;
        case KEY_BACKSPACE: {
            i32 e = pane_first_selected_entry(p);
            if (ready && e >= 0) app_sftp_fb_action(app, st, pi, FB_ACT_DELETE, e);
            return true;
        }
        case KEY_R: if (shift) return false; if (ready && p->fb.cwd[0]) { fb_refresh_keep(&p->fb, NULL); p->view_dirty = true; } return true;
        case KEY_PERIOD: if (!shift) return false; p->show_hidden = !p->show_hidden; p->view_dirty = true; return true;
        case KEY_H: if (!shift) return false; if (ready && p->home[0]) pane_go(app, tab, pi, p->home, true); return true;
        case KEY_F: if (shift) return false; filter_focus(app, st, pi); return true;
        default: break;
        }
    }
    return false;
}

bool sftp_tab_char(AppState *app, u32 cp, u32 mods) {
    SftpTab *st = active_sftp(app);
    if (st && app_modal_open(app)) return false;
    if (!st || st->menu_open) return st != NULL;
    if (mods & (MOD_SUPER | MOD_CTRL)) return true;
    if (cp < 32 || cp == 127) return true;
    i32 pi = st->focus == SFTP_PANE_LOCAL ? SFTP_PANE_LOCAL : SFTP_PANE_REMOTE;
    SftpPane *p = &st->pane[pi];
    /* Typing in a list filters it: the field opens with the first letter. */
    if (app->te_owner != (const void *)p->filter) {
        if (cp == ' ') return true;
        filter_focus(app, st, pi);
    }
    TextEdit e = app_edit(app, p->filter, &p->filter_len, SFTP_FILTER_CAP);
    textedit_insert_cp(&e, &app->te_undo, cp);
    app_edit_done(app, p->filter_len);
    p->view_dirty = true;
    return true;
}

bool sftp_tab_drop_files(AppState *app, f32 x, f32 y, const char *const *paths, i32 count) {
    SftpTab *st = active_sftp(app);
    if (!st || !st->geom_valid || count <= 0 || app_modal_open(app)) return false;
    Tab *tab = &app->tabs[app->active_tab];
    SftpHit h = hit_at(st, x, y, false);
    if (h.pane < 0) return false;
    if (!pane_ready(app, tab, h.pane)) {
        pane_say(&st->pane[h.pane], SFTP_MSG_ERROR, "Not connected to the host");
        return true;
    }
    SftpPane *p = &st->pane[h.pane];
    char dest[FB_MAX_PATH * 2];
    const FileEntry *e = h.kind == SFTP_HIT_ROW ? pane_entry(p, h.index) : NULL;
    if (e && e->is_dir && !entry_is_up(e)) fb_entry_path(&p->fb, e, dest, sizeof dest);
    else snprintf(dest, sizeof dest, "%s", p->fb.cwd);
    st->focus = h.pane;
    /* A drop of several files arrives as several events: gather them. One
     * aimed somewhere else first sends what was gathered. */
    if (st->queue_n > 0 && (st->queue_to != h.pane || strcmp(st->queue_dest, dest))) {
        /* Only to a side that is still there: aimed at the host, the queue's
         * folder is a REMOTE path, and with the link gone the pane is local
         * again — the files went into that path string on this disk. */
        if (!app_sftp_busy(app) && pane_ready(app, tab, st->queue_to))
            app_sftp_transfer_paths(app, st, (const char *const *)st->queue, st->queue_n,
                                    st->queue_to, st->queue_dest);
        else if (!pane_ready(app, tab, st->queue_to))
            pane_say(&st->pane[st->queue_to], SFTP_MSG_ERROR, "Not connected to the host");
        queue_clear(st);
    }
    st->queue_to = h.pane;
    snprintf(st->queue_dest, sizeof st->queue_dest, "%s", dest);
    for (i32 i = 0; i < count; i++) {
        if (!paths[i] || !paths[i][0] || st->queue_n >= FB_CLIP_MAX) continue;
        if (st->queue_n >= st->queue_cap) {
            i32 cap = st->queue_cap ? st->queue_cap * 2 : 8;
            char **q = realloc(st->queue, (usize)cap * sizeof *q);
            if (!q) break;
            st->queue = q;
            st->queue_cap = cap;
        }
        char *dup = strdup(paths[i]);
        if (dup) st->queue[st->queue_n++] = dup;
    }
    if (app_sftp_busy(app)) pane_say(p, SFTP_MSG_INFO, "Waiting for the transfer that is running");
    platform_wake_event_loop();
    return true;
}
