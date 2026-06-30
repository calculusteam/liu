/*
 * Liu - Sites (dev-server manager) panel — UI layer.
 * See sites_ui.h for the overview.
 */
#include "ui/sites_ui.h"
#include "ui/ui.h"
#include "ui/anim.h"
#include "ui/chrome_palette.h"
#include "ui/layout.h"
#include "core/sites.h"
#include "core/utf8.h"
#include "terminal/terminal.h"
#include "terminal/url.h"
#include "platform/platform.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

/* Footer actions, left-to-right. */
enum { SACT_START, SACT_STOP, SACT_RESTART, SACT_REMOVE, SACT_BROWSER, SACT_COUNT };
static const char *const SACT_LABEL[SACT_COUNT] = {
    "Start", "Stop", "Restart", "Remove", "Browser"
};

/* Per-frame alpha for the whole panel (set at the top of render). */
static f32 g_sites_alpha = 1.0f;

/* ------------------------------------------------------------------------- */

static Color tint(Color c) { c.a *= g_sites_alpha; return c; }

static void s_text(Renderer *r, const char *str, f32 x, f32 y, Color fg,
                   f32 adv, i32 maxchars) {
    fg = tint(fg);
    const u8 *p = (const u8 *)str;
    i32 drawn = 0;
    if (maxchars < 0) maxchars = 100000;
    while (*p && drawn < maxchars) {
        u32 cp;
        u32 c = utf8_decode(p, 4, &cp);
        if (c == 0) { p++; continue; }
        if (cp == '\n') break;   /* single-line: stop at a newline */
        if (cp >= 32) { renderer_push_glyph(r, x, y, cp, fg); x += adv; drawn++; }
        p += c;
    }
}

/* Draw plain (already ANSI-stripped) text wrapped to `cols`, showing the last
 * `rows` wrapped lines — the failure tail's end, where the error sits. Keeps a
 * ring of only the most recent line starts so a long tail costs O(rows) memory. */
static void draw_log_tail(Renderer *r, const char *txt, f32 x, f32 y,
                          i32 cols, i32 rows, f32 cw, f32 ch, Color fg) {
    if (!txt || !*txt || cols < 1 || rows < 1) return;
    if (rows > 256) rows = 256;
    const char *starts[256];
    i32 head = 0, count = 1;
    starts[0] = txt;
    i32 col = 0;
    const u8 *p = (const u8 *)txt;
    while (*p) {
        u32 cp;
        u32 adv = utf8_decode(p, 4, &cp);
        if (adv == 0) { p++; continue; }
        p += adv;
        bool wrap = false;
        if (cp == '\n')      { col = 0; wrap = true; }
        else if (++col >= cols) { col = 0; wrap = true; }
        if (wrap) {
            head = (head + 1) % rows;
            if (count < rows) count++;
            starts[head] = (const char *)p;
        }
    }
    i32 first = ((head - count + 1) % rows + rows) % rows;
    f32 ly = y;
    for (i32 k = 0; k < count; k++) {
        s_text(r, starts[(first + k) % rows], x, ly, fg, cw, cols);
        ly += ch;
    }
}

static i32 s_strchars(const char *str) {
    i32 n = 0;
    const u8 *p = (const u8 *)str;
    while (*p) {
        u32 cp;
        u32 c = utf8_decode(p, 4, &cp);
        if (c == 0) { p++; continue; }
        n++; p += c;
    }
    return n;
}

static Color status_color(SiteStatus st, const Theme *t) {
    switch (st) {
        case SITE_RUNNING:  return (Color){0.30f, 0.78f, 0.40f, 1.0f};
        case SITE_STARTING: return (Color){0.95f, 0.70f, 0.25f, 1.0f};
        case SITE_EXITED:   return (Color){0.55f, 0.58f, 0.62f, 1.0f};
        case SITE_FAILED:   return (Color){0.85f, 0.42f, 0.42f, 1.0f};
        default:            return t ? (Color){t->fg.r * 0.4f, t->fg.g * 0.4f,
                                               t->fg.b * 0.4f, 1.0f}
                                     : (Color){0.4f, 0.4f, 0.4f, 1.0f};
    }
}

static const char *status_text(const Site *s) {
    switch (s->status) {
        case SITE_RUNNING:  return "running";
        case SITE_STARTING: return "starting";
        case SITE_EXITED:   return "exited";
        case SITE_FAILED:   return "failed";
        default:            return "stopped";
    }
}

static void path_display(const char *path, char *out, usize osz) {
    const char *home = getenv("HOME");
    if (home && *home) {
        usize hl = strlen(home);
        if (strncmp(path, home, hl) == 0) {
            snprintf(out, osz, "~%s", path + hl);
            return;
        }
    }
    snprintf(out, osz, "%s", path);
}

static void uptime_str(const Site *s, char *out, usize osz) {
    if ((s->status != SITE_RUNNING && s->status != SITE_STARTING) ||
        s->started_at <= 0.0) { out[0] = '\0'; return; }
    i64 secs = (i64)(platform_time_sec() - s->started_at);
    if (secs < 0) secs = 0;
    if (secs < 60)        snprintf(out, osz, "%llds", (long long)secs);
    else if (secs < 3600) snprintf(out, osz, "%lldm", (long long)(secs / 60));
    else snprintf(out, osz, "%lldh%lldm",
                  (long long)(secs / 3600), (long long)((secs % 3600) / 60));
}

/* ------------------------------------------------------------------------- */

typedef struct {
    f32 dpi, ui_cw, ui_ch;
    f32 ppx, ppy, pw, ph;
    f32 header_y, header_h;
    f32 body_y, body_h;
    f32 list_x, list_w;
    f32 log_x, log_w;
    f32 footer_y, footer_h;
    f32 row_h;
    f32 row_pad;    /* top/bottom padding inside a list row */
    f32 line_step;  /* vertical advance between the 3 stacked text lines */
    f32 glyph_h;    /* real rendered UI glyph height (font-aspect aware) */
} SitesLayout;

static void sites_compute_layout(AppState *app, f32 scale, f32 yoff, SitesLayout *L) {
    f32 dpi = app->dpi_scale;
    L->dpi   = dpi;
    L->ui_cw = 8.0f * dpi;
    L->ui_ch = 16.0f * dpi;

    f32 margin_x = 40.0f * dpi;
    f32 top_y    = app->tab_bar_height + 24.0f * dpi;
    f32 margin_b = 24.0f * dpi + app->status_bar_height;

    f32 pw = (f32)app->fb_width - 2.0f * margin_x;
    f32 max_pw = 1100.0f * dpi;
    if (pw > max_pw) pw = max_pw;
    f32 ph = (f32)app->fb_height - top_y - margin_b;
    if (pw < 320.0f * dpi) pw = 320.0f * dpi;
    if (ph < 200.0f * dpi) ph = 200.0f * dpi;

    f32 ppx = ((f32)app->fb_width - pw) * 0.5f;
    f32 ppy = top_y;

    /* Modal entrance transform. */
    ppx += (pw - pw * scale) * 0.5f;
    ppy += (ph - ph * scale) * 0.5f - yoff;
    pw  *= scale;
    ph  *= scale;

    f32 header_h = SITES_HEADER_H_PT * dpi;
    f32 footer_h = SITES_FOOTER_H_PT * dpi;
    f32 list_w   = SITES_LIST_W_PT   * dpi;
    if (list_w > pw * 0.45f) list_w = pw * 0.45f;

    L->ppx = ppx; L->ppy = ppy; L->pw = pw; L->ph = ph;
    L->header_y = ppy; L->header_h = header_h;
    L->body_y = ppy + header_h;
    L->body_h = ph - header_h - footer_h;
    L->list_x = ppx; L->list_w = list_w;
    L->log_x = ppx + list_w; L->log_w = pw - list_w;
    L->footer_y = ppy + ph - footer_h; L->footer_h = footer_h;

    /* The renderer draws UI glyphs at the font's true aspect
     * (ui_cw * cell_h/cell_w), which is taller than the 1:2 (8x16) cell the
     * rest of the layout assumes — JetBrains Mono is ~1:2.2. Size each list
     * row from the real glyph height so the three stacked lines
     * (name / path / status) never spill past the row (same pattern as the
     * file-browser tile labels). */
    f32 fcw = app->renderer.font.cell_width;
    f32 fch = app->renderer.font.cell_height;
    L->glyph_h   = (fcw > 0.0f) ? L->ui_cw * (fch / fcw) : L->ui_ch;
    L->row_pad   = 7.0f * dpi;
    L->line_step = L->glyph_h + 2.0f * dpi;
    L->row_h     = 2.0f * L->row_pad + L->glyph_h + 2.0f * L->line_step;
}

/* Header "+ Add Folder" button rect. */
static void sites_add_btn(const SitesLayout *L, f32 *x, f32 *y, f32 *w, f32 *h) {
    f32 bw = ((f32)((i32)strlen("+ Add Folder") + 3)) * L->ui_cw;
    f32 bh = 26.0f * L->dpi;
    *w = bw; *h = bh;
    *x = L->ppx + L->pw - bw - 14.0f * L->dpi;
    *y = L->header_y + (L->header_h - bh) * 0.5f;
}

/* Footer action button rect for index `idx` (0..SACT_COUNT-1). */
static void sites_footer_btn(const SitesLayout *L, i32 idx,
                             f32 *x, f32 *y, f32 *w, f32 *h) {
    f32 bh  = 28.0f * L->dpi;
    f32 gap = 8.0f * L->dpi;
    f32 cx  = L->ppx + 12.0f * L->dpi;
    for (i32 i = 0; i < idx; i++) {
        f32 bw = ((f32)((i32)strlen(SACT_LABEL[i]) + 3)) * L->ui_cw;
        cx += bw + gap;
    }
    f32 bw = ((f32)((i32)strlen(SACT_LABEL[idx]) + 3)) * L->ui_cw;
    *x = cx; *w = bw; *h = bh;
    *y = L->footer_y + (L->footer_h - bh) * 0.5f;
}

static bool pt_in(f32 px, f32 py, f32 x, f32 y, f32 w, f32 h) {
    return px >= x && px < x + w && py >= y && py < y + h;
}

/* Map a point inside the log pane to a clickable URL in the live log terminal.
 * Mirrors the geometry used by the terminal branch of render_sites_panel: the
 * grid origin is (log_x + pad, body_y + pad) at the font's real cell size. Only
 * valid while that branch is actually on screen (running site with a terminal —
 * not the add-form or the failure panel). */
static bool sites_log_url_at(AppState *app, const SitesLayout *L,
                             f32 mx, f32 my, TermURL *out) {
    Site *sel = sites_get(&app->site_mgr, app->sites.selected);
    if (!sel || !sel->term) return false;
    if (app->sites.addform_active || sel->status == SITE_FAILED) return false;
    f32 pad = SITES_LOG_PAD_PT * L->dpi;
    f32 ox = L->log_x + pad;
    f32 oy = L->body_y + pad;
    f32 cw = app->renderer.font.cell_width;
    f32 ch = app->renderer.font.cell_height;
    if (cw <= 0.0f || ch <= 0.0f || mx < ox || my < oy) return false;
    i32 col = (i32)((mx - ox) / cw);
    i32 row = (i32)((my - oy) / ch);
    return url_detect_at(sel->term, col, row, out);
}

static bool sact_enabled(const Site *s, i32 act) {
    if (!s) return false;
    bool running = (s->status == SITE_RUNNING || s->status == SITE_STARTING);
    switch (act) {
        case SACT_START:   return !running;
        case SACT_STOP:    return running;
        case SACT_RESTART: return true;
        case SACT_REMOVE:  return true;
        case SACT_BROWSER: return running && site_effective_port(s) > 0;
        default:           return false;
    }
}

/* ------------------------------------------------------------------------- */

void sites_ui_init(AppState *app) {
    SiteManagerUI *u = &app->sites;
    u->active = false;
    u->selected = -1;
    u->hover_row = -1;
    u->hover_action = -1;
    u->list_scroll_px = 0.0f;
    u->addform_active = false;
    u->addform_site = -1;
    u->addform_buf[0] = '\0';
    u->addform_len = 0;
    u->log_cache_term = NULL;
}

static void sites_ensure_visible(AppState *app) {
    SitesLayout L;
    sites_compute_layout(app, 1.0f, 0.0f, &L);
    if (app->sites.selected < 0) return;
    f32 row_top = L.row_h * (f32)app->sites.selected;
    f32 row_bot = row_top + L.row_h;
    if (row_top < app->sites.list_scroll_px)
        app->sites.list_scroll_px = row_top;
    else if (row_bot > app->sites.list_scroll_px + L.body_h)
        app->sites.list_scroll_px = row_bot - L.body_h;
    if (app->sites.list_scroll_px < 0) app->sites.list_scroll_px = 0;
}

static void sites_do_add_folder(AppState *app) {
    const char *dir = platform_open_folder_dialog("Add a project folder");
    if (!dir || !dir[0]) return;
    SiteDetect d;
    sites_detect_package_json(dir, &d);
    Site *s = sites_add(&app->site_mgr, d.name, dir, d.dev_cmd);
    if (!s) { app_show_toast(app, "Site list is full"); return; }
    app->sites.selected = app->site_mgr.count - 1;
    app->sites.addform_active = true;
    app->sites.addform_site = app->sites.selected;
    snprintf(app->sites.addform_buf, sizeof app->sites.addform_buf, "%s", s->command);
    app->sites.addform_len = (i32)strlen(app->sites.addform_buf);
    sites_ensure_visible(app);
}

/* After (re)starting a site, make its log terminal the visible pane right
 * away. Two things have to happen or the freshly-started server's output
 * doesn't surface until the panel is closed and reopened:
 *   1. Drop the inline add-form — otherwise the command editor keeps the log
 *      pane hidden when Start is pressed while the form is up.
 *   2. Invalidate the log-pane cache so the next frame rebinds and FULLY
 *      rebuilds against the (possibly brand-new) terminal. While the panel is
 *      statically open the grid origin never moves, so the cache otherwise
 *      only does a full rebuild when the modal's open animation shifts the
 *      origin — which is exactly why reopening the panel "fixed" it. */
static void sites_reveal_log(AppState *app) {
    app->sites.addform_active   = false;
    app->sites.log_cache_term   = NULL;
    app->sites.log_cache.all_rows_dirty = true;
}

static void sites_addform_commit(AppState *app, bool start) {
    Site *s = sites_get(&app->site_mgr, app->sites.addform_site);
    if (s) snprintf(s->command, sizeof s->command, "%s", app->sites.addform_buf);
    app->sites.addform_active = false;
    if (s && start) { site_start(&app->site_mgr, s); sites_reveal_log(app); }
}

static void sites_do_action(AppState *app, i32 act) {
    Site *s = sites_get(&app->site_mgr, app->sites.selected);
    if (!s || !sact_enabled(s, act)) return;
    switch (act) {
        case SACT_START:
            /* Start pressed while the inline add-form is up for this site:
             * persist the typed command first so the button matches Enter. */
            if (app->sites.addform_active &&
                app->sites.addform_site == app->sites.selected)
                snprintf(s->command, sizeof s->command, "%s", app->sites.addform_buf);
            site_start(&app->site_mgr, s);
            sites_reveal_log(app);
            break;
        case SACT_STOP:    site_stop(s); break;
        case SACT_RESTART: site_restart(&app->site_mgr, s); sites_reveal_log(app); break;
        case SACT_REMOVE:
            sites_remove(&app->site_mgr, app->sites.selected);
            app->sites.log_cache_term = NULL;
            if (app->sites.selected >= app->site_mgr.count)
                app->sites.selected = app->site_mgr.count - 1;
            /* A pending add-form holds a now-stale slot index once the array
             * shifts down on removal; cancel it so a later Enter can't commit
             * the typed command into the wrong site. */
            app->sites.addform_active = false;
            app->sites.addform_site = -1;
            break;
        case SACT_BROWSER: {
            i32 port = site_effective_port(s);
            if (port > 0) {
                char url[64];
                snprintf(url, sizeof url, "http://localhost:%d", port);
                platform_open_url(url);
            }
            break;
        }
        default: break;
    }
}

/* ------------------------------------------------------------------------- */

bool sites_handle_click(AppState *app, f32 mx, f32 my) {
    if (!app->sites.active) return false;
    SitesLayout L;
    sites_compute_layout(app, 1.0f, 0.0f, &L);

    /* Header: Add Folder button. */
    f32 ax, ay, aw, ah;
    sites_add_btn(&L, &ax, &ay, &aw, &ah);
    if (pt_in(mx, my, ax, ay, aw, ah)) { sites_do_add_folder(app); return true; }

    /* Footer: action buttons. */
    for (i32 i = 0; i < SACT_COUNT; i++) {
        f32 bx, by, bw, bh;
        sites_footer_btn(&L, i, &bx, &by, &bw, &bh);
        if (pt_in(mx, my, bx, by, bw, bh)) { sites_do_action(app, i); return true; }
    }

    /* List: select a row. */
    if (pt_in(mx, my, L.list_x, L.body_y, L.list_w, L.body_h) &&
        app->site_mgr.count > 0) {
        i32 row = (i32)((my - L.body_y + app->sites.list_scroll_px) / L.row_h);
        if (row >= 0 && row < app->site_mgr.count) {
            app->sites.selected = row;
            if (app->sites.addform_active && app->sites.addform_site != row)
                app->sites.addform_active = false;
        }
        return true;
    }

    /* Log pane: click a URL in the live output to open it in the browser. */
    if (pt_in(mx, my, L.log_x, L.body_y, L.log_w, L.body_h)) {
        TermURL link;
        if (sites_log_url_at(app, &L, mx, my, &link))
            platform_open_url(link.url);
        return true;
    }

    /* Clicks inside the panel are swallowed; outside clicks fall through. */
    if (pt_in(mx, my, L.ppx, L.ppy, L.pw, L.ph)) return true;
    return false;
}

static void addform_backspace(AppState *app) {
    i32 len = app->sites.addform_len;
    char *b = app->sites.addform_buf;
    if (len <= 0) return;
    do { len--; } while (len > 0 && ((u8)b[len] & 0xC0) == 0x80);
    b[len] = '\0';
    app->sites.addform_len = len;
}

bool sites_handle_key(AppState *app, u32 key, u32 mods) {
    if (!app->sites.active) return false;
    SiteManagerUI *u = &app->sites;
    (void)mods;

    if (u->addform_active) {
        if (key == KEY_ESCAPE)    return false;   /* caller closes the form */
        if (key == KEY_ENTER)     { sites_addform_commit(app, true); return true; }
        if (key == KEY_BACKSPACE) { addform_backspace(app); return true; }
        return true;                               /* swallow while editing */
    }

    i32 n = app->site_mgr.count;
    switch (key) {
        case KEY_UP:
            if (n > 0) { u->selected = (u->selected <= 0) ? 0 : u->selected - 1;
                         sites_ensure_visible(app); }
            return true;
        case KEY_DOWN:
            if (n > 0) { u->selected = (u->selected < 0) ? 0
                                       : (u->selected < n - 1 ? u->selected + 1 : u->selected);
                         sites_ensure_visible(app); }
            return true;
        case KEY_ENTER: {
            Site *s = sites_get(&app->site_mgr, u->selected);
            if (s) { site_start(&app->site_mgr, s); sites_reveal_log(app); }
            return true;
        }
        default: return false;
    }
}

bool sites_handle_char(AppState *app, u32 codepoint) {
    if (!app->sites.active || !app->sites.addform_active) return false;
    if (codepoint < 32 || codepoint == 127) return true;

    /* UTF-8 encode into the command buffer via the shared utility. */
    u8 enc[4];
    i32 n = (i32)utf8_encode(codepoint, enc);
    if (n == 0) return true;          /* invalid codepoint (surrogate / out of range) */
    if ((usize)(app->sites.addform_len + n) < sizeof(app->sites.addform_buf) - 1) {
        memcpy(app->sites.addform_buf + app->sites.addform_len, enc, (usize)n);
        app->sites.addform_len += n;
        app->sites.addform_buf[app->sites.addform_len] = '\0';
    }
    return true;
}

void sites_handle_scroll(AppState *app, f32 dy, bool precise, f32 mx, f32 my) {
    if (!app->sites.active) return;
    SitesLayout L;
    sites_compute_layout(app, 1.0f, 0.0f, &L);

    static f32 s_log_accum = 0.0f;
    if (pt_in(mx, my, L.log_x, L.body_y, L.log_w, L.body_h)) {
        Site *s = sites_get(&app->site_mgr, app->sites.selected);
        if (!s || !s->term) return;
        if (precise) {
            s_log_accum += dy;
            while (s_log_accum >= 28.0f)  { terminal_scroll_up(s->term, 1);   s_log_accum -= 28.0f; }
            while (s_log_accum <= -28.0f) { terminal_scroll_down(s->term, 1); s_log_accum += 28.0f; }
        } else {
            if (dy > 0)      terminal_scroll_up(s->term, (i32)dy);
            else if (dy < 0) terminal_scroll_down(s->term, (i32)(-dy));
        }
        return;
    }

    /* List scroll. */
    f32 delta = precise ? dy : dy * L.ui_ch;
    app->sites.list_scroll_px -= delta;
    f32 content_h = L.row_h * (f32)app->site_mgr.count;
    f32 max_scroll = content_h - L.body_h;
    if (max_scroll < 0) max_scroll = 0;
    if (app->sites.list_scroll_px < 0) app->sites.list_scroll_px = 0;
    if (app->sites.list_scroll_px > max_scroll) app->sites.list_scroll_px = max_scroll;
}

void sites_handle_mouse_move(AppState *app, f32 mx, f32 my) {
    if (!app->sites.active) return;
    SitesLayout L;
    sites_compute_layout(app, 1.0f, 0.0f, &L);

    app->sites.hover_row = -1;
    app->sites.hover_action = -1;
    bool pointer = false;

    if (pt_in(mx, my, L.list_x, L.body_y, L.list_w, L.body_h)) {
        i32 row = (i32)((my - L.body_y + app->sites.list_scroll_px) / L.row_h);
        if (row >= 0 && row < app->site_mgr.count) {
            app->sites.hover_row = row;
            pointer = true;
        }
    }
    for (i32 i = 0; i < SACT_COUNT; i++) {
        f32 bx, by, bw, bh;
        sites_footer_btn(&L, i, &bx, &by, &bw, &bh);
        if (pt_in(mx, my, bx, by, bw, bh)) {
            app->sites.hover_action = i;
            Site *sel = sites_get(&app->site_mgr, app->sites.selected);
            if (sact_enabled(sel, i)) pointer = true;
            break;
        }
    }

    /* Add Folder button + clickable URLs in the log feel "linky". */
    f32 ax, ay, aw, ah;
    sites_add_btn(&L, &ax, &ay, &aw, &ah);
    if (pt_in(mx, my, ax, ay, aw, ah)) pointer = true;
    if (!pointer) {
        TermURL link;
        if (sites_log_url_at(app, &L, mx, my, &link)) pointer = true;
    }

    platform_set_cursor(pointer ? CURSOR_POINTER : CURSOR_DEFAULT);
}

/* ------------------------------------------------------------------------- */

void render_sites_panel(AppState *app) {
    Renderer *r = &app->renderer;
    SiteManagerUI *u = &app->sites;

    f32 scale, alpha, yoff;
    bool show = modal_anim_progress(u->active, &u->open_anim, &u->close_anim,
                                    &u->was_open, app->dpi_scale,
                                    MODAL_OPEN_DUR_LARGE, &scale, &alpha, &yoff);
    if (!show) return;
    g_sites_alpha = alpha;

    const Theme *t = app->config.theme;
    Color panel_bg = t ? t->bg : (Color){0.05f, 0.05f, 0.06f, 1.0f};
    Color text_fg  = t ? t->fg : (Color){0.9f, 0.9f, 0.92f, 1.0f};
    Color text_dim = (Color){text_fg.r * 0.6f, text_fg.g * 0.6f, text_fg.b * 0.6f, 1.0f};
    Color border   = t ? t->border : (Color){0.12f, 0.12f, 0.14f, 1.0f};
    if (border.a < 0.05f) border.a = 1.0f;
    Color accent   = theme_ui_accent(t);

    SitesLayout L;
    sites_compute_layout(app, scale, yoff, &L);
    f32 dpi = L.dpi, ui_cw = L.ui_cw, ui_ch = L.ui_ch;
    renderer_set_ui_scale(r, ui_cw, ui_ch);

    /* --- Dim backdrop --- */
    renderer_draw_rect(r, 0, 0, (f32)app->fb_width, (f32)app->fb_height,
                       (Color){0, 0, 0, 0.5f * alpha});
    renderer_flush_rects(r);

    /* --- Panel body --- */
    f32 pr = 12.0f * dpi;
    renderer_draw_rrect_bordered(r, L.ppx, L.ppy, L.pw, L.ph,
                                 tint(panel_bg), tint(border), fmaxf(1.0f, dpi),
                                 pr, pr, pr, pr, 24.0f * dpi, 0.35f * alpha,
                                 0.0f, 6.0f * dpi);
    renderer_flush_rrects(r);

    Site *sel = sites_get(&app->site_mgr, u->selected);

    /* --- rrect layer: add button, list rows, footer buttons, status dots --- */
    /* Add Folder button (primary accent). */
    f32 ax, ay, aw, ah;
    sites_add_btn(&L, &ax, &ay, &aw, &ah);
    renderer_draw_rrect_simple(r, ax, ay, aw, ah, tint(accent), 6.0f * dpi);

    /* List rows: selection + hover backgrounds. */
    for (i32 i = 0; i < app->site_mgr.count; i++) {
        f32 ry = L.body_y + (f32)i * L.row_h - u->list_scroll_px;
        if (ry + L.row_h < L.body_y || ry > L.body_y + L.body_h) continue;
        if (i == u->selected) {
            Color selbg = accent; selbg.a = 0.16f;
            renderer_draw_rrect_simple(r, L.list_x + 4.0f * dpi, ry + 2.0f * dpi,
                                       L.list_w - 8.0f * dpi, L.row_h - 4.0f * dpi,
                                       tint(selbg), 6.0f * dpi);
        } else if (i == u->hover_row) {
            Color hv = text_fg; hv.a = 0.06f;
            renderer_draw_rrect_simple(r, L.list_x + 4.0f * dpi, ry + 2.0f * dpi,
                                       L.list_w - 8.0f * dpi, L.row_h - 4.0f * dpi,
                                       tint(hv), 6.0f * dpi);
        }
        /* Status dot. */
        Color dot = status_color(app->site_mgr.sites[i].status, t);
        f32 dd = 8.0f * dpi;
        renderer_draw_rrect_simple(r, L.list_x + 14.0f * dpi,
                                   ry + L.row_h * 0.5f - dd * 0.5f,
                                   dd, dd, tint(dot), dd * 0.5f);
    }

    /* Footer action buttons. */
    for (i32 i = 0; i < SACT_COUNT; i++) {
        f32 bx, by, bw, bh;
        sites_footer_btn(&L, i, &bx, &by, &bw, &bh);
        bool en = sact_enabled(sel, i);
        bool hov = (u->hover_action == i) && en;
        Color fill;
        if (hov) { fill = accent; fill.a = 0.9f; }
        else if (en) { fill = text_fg; fill.a = 0.10f; }
        else { fill = text_fg; fill.a = 0.04f; }
        renderer_draw_rrect_simple(r, bx, by, bw, bh, tint(fill), 6.0f * dpi);
    }
    renderer_flush_rrects(r);

    /* --- flat rect layer: dividers --- */
    Color div = border; div.a = 0.6f;
    renderer_draw_rect(r, L.ppx + pr * 0.5f, L.header_y + L.header_h - fmaxf(1.0f, dpi),
                       L.pw - pr, fmaxf(1.0f, dpi), tint(div));
    renderer_draw_rect(r, L.list_x + L.list_w, L.body_y,
                       fmaxf(1.0f, dpi), L.body_h, tint(div));
    renderer_draw_rect(r, L.ppx + pr * 0.5f, L.footer_y,
                       L.pw - pr, fmaxf(1.0f, dpi), tint(div));
    renderer_flush_rects(r);

    /* --- glyph layer --- */
    /* Header: title + running/stopped summary. */
    s_text(r, "Sites", L.ppx + 16.0f * dpi, L.header_y + (L.header_h - ui_ch) * 0.5f,
           text_fg, ui_cw, -1);
    {
        i32 running = sites_running_count(&app->site_mgr);
        i32 stopped = app->site_mgr.count - running;
        char summary[64];
        snprintf(summary, sizeof summary, "%d running · %d stopped", running, stopped);
        f32 sx = L.ppx + 16.0f * dpi + (f32)(strlen("Sites") + 3) * ui_cw;
        s_text(r, summary, sx, L.header_y + (L.header_h - ui_ch) * 0.5f, text_dim, ui_cw, -1);
    }
    /* Add Folder label. */
    s_text(r, "+ Add Folder", ax + 1.5f * ui_cw, ay + (ah - ui_ch) * 0.5f,
           chrome_legible_on(accent), ui_cw, -1);

    /* List rows: name, path, status/port/uptime. */
    for (i32 i = 0; i < app->site_mgr.count; i++) {
        f32 ry = L.body_y + (f32)i * L.row_h - u->list_scroll_px;
        if (ry + L.row_h < L.body_y || ry > L.body_y + L.body_h) continue;
        const Site *s = &app->site_mgr.sites[i];
        f32 tx = L.list_x + 30.0f * dpi;
        f32 avail = (L.list_w - 30.0f * dpi - 10.0f * dpi);
        i32 maxc = (i32)(avail / ui_cw);

        f32 ly = ry + L.row_pad;
        s_text(r, s->name[0] ? s->name : "(unnamed)", tx, ly,
               text_fg, ui_cw, maxc);

        char disp[1100];
        path_display(s->path, disp, sizeof disp);
        s_text(r, disp, tx, ly + L.line_step, text_dim, ui_cw, maxc);

        char meta[96];
        i32 port = site_effective_port(s);
        char up[24]; uptime_str(s, up, sizeof up);
        if (port > 0 && up[0])
            snprintf(meta, sizeof meta, ":%d  %s  %s", port, status_text(s), up);
        else if (port > 0)
            snprintf(meta, sizeof meta, ":%d  %s", port, status_text(s));
        else
            snprintf(meta, sizeof meta, "%s", status_text(s));
        Color mc = (s->status == SITE_FAILED)
                   ? (Color){0.85f, 0.42f, 0.42f, 1.0f} : text_dim;
        s_text(r, meta, tx, ly + 2.0f * L.line_step, mc, ui_cw, maxc);
    }

    /* Footer: action labels + Esc hint. */
    for (i32 i = 0; i < SACT_COUNT; i++) {
        f32 bx, by, bw, bh;
        sites_footer_btn(&L, i, &bx, &by, &bw, &bh);
        bool en = sact_enabled(sel, i);
        bool hov = (u->hover_action == i) && en;
        Color fg = hov ? chrome_legible_on(accent) : (en ? text_fg : text_dim);
        s_text(r, SACT_LABEL[i], bx + 1.5f * ui_cw, by + (bh - ui_ch) * 0.5f,
               fg, ui_cw, -1);
    }
    s_text(r, "Esc", L.ppx + L.pw - 5.0f * ui_cw, L.footer_y + (L.footer_h - ui_ch) * 0.5f,
           text_dim, ui_cw, -1);
    renderer_flush_glyphs(r);

    /* --- Right pane: log terminal, add-form, or empty state --- */
    f32 pad = SITES_LOG_PAD_PT * dpi;
    f32 log_in_x = L.log_x + pad;
    f32 log_in_y = L.body_y + pad;
    f32 log_in_w = L.log_w - 2.0f * pad;
    f32 log_in_h = L.body_h - 2.0f * pad;

    if (u->addform_active && sel) {
        /* Inline command editor for a freshly added site. */
        f32 fy = log_in_y + 40.0f * dpi;
        f32 fh = 30.0f * dpi;
        s_text(r, "Start command:", log_in_x, log_in_y, text_fg, ui_cw, -1);
        renderer_draw_rrect_bordered(r, log_in_x, fy, log_in_w, fh,
                                     tint((Color){text_fg.r, text_fg.g, text_fg.b, 0.06f}),
                                     tint(accent), fmaxf(1.0f, dpi),
                                     5.0f * dpi, 5.0f * dpi, 5.0f * dpi, 5.0f * dpi,
                                     0, 0, 0, 0);
        renderer_flush_rrects(r);
        s_text(r, u->addform_buf, log_in_x + 0.7f * ui_cw, fy + (fh - ui_ch) * 0.5f,
               text_fg, ui_cw, (i32)(log_in_w / ui_cw));
        s_text(r, "Enter to start · Esc to keep stopped",
               log_in_x, fy + fh + 12.0f * dpi, text_dim, ui_cw, -1);
        renderer_flush_glyphs(r);
    } else if (sel && sel->status == SITE_FAILED) {
        /* Failure panel: exit code + the captured (ANSI-stripped) error tail, so
         * you can see WHY a run died even when the dev tool cleared the screen or
         * used the alternate buffer (which leaves the live log terminal blank). */
        Color red = (Color){0.85f, 0.42f, 0.42f, 1.0f};
        char hdr[80];
        if (sel->exit_code >= 0)
            snprintf(hdr, sizeof hdr, "Failed · exit code %d", sel->exit_code);
        else
            snprintf(hdr, sizeof hdr, "Failed to start");
        s_text(r, hdr, log_in_x, log_in_y, red, ui_cw, -1);

        f32 body_y  = log_in_y + L.glyph_h + 10.0f * dpi;
        f32 avail_h = log_in_y + log_in_h - body_y;
        i32 cols = (i32)(log_in_w / ui_cw);
        i32 rows = (L.line_step > 0.0f) ? (i32)(avail_h / L.line_step) : 1;
        if (cols < 1) cols = 1;
        if (rows < 1) rows = 1;
        if (sel->log_tail[0]) {
            Color body = (Color){text_fg.r * 0.82f, text_fg.g * 0.82f,
                                  text_fg.b * 0.82f, 1.0f};
            draw_log_tail(r, sel->log_tail, log_in_x, body_y, cols, rows,
                          ui_cw, L.line_step, body);
        } else {
            s_text(r, "No output was captured before the process exited.",
                   log_in_x, body_y, text_dim, ui_cw, cols);
        }
        renderer_flush_glyphs(r);
    } else if (sel && sel->term && u->active) {
        /* The log pane is a real Terminal grid: it must render at the font's
         * true cell metrics. Clear the UI-chrome scale override (set at the top
         * of this function for the panel's own 8x16 chrome text) first —
         * otherwise renderer_flush_glyphs draws each glyph quad at ui_cell_w
         * while render_terminal_pane steps the columns by font.cell_width,
         * which spreads the monospace output out with loose letter-spacing.
         *
         * Gated on u->active so the live terminal is skipped during the CLOSE
         * transition: render_terminal_pane ignores the panel's fade alpha, so
         * its opaque glyphs would otherwise hang in the air while the box
         * dissolves. Letting the panel background fade out on its own is clean,
         * and the open animation is short enough that not painting it on the way
         * in is imperceptible. */
        renderer_reset_ui_scale(r);
        f32 grid_y = log_in_y;
        f32 cw = r->font.cell_width, ch = r->font.cell_height;
        i32 cols = (cw > 0) ? (i32)(log_in_w / cw) : 80;
        i32 rows = (ch > 0) ? (i32)((log_in_y + log_in_h - grid_y) / ch) : 24;
        if (cols < 1) cols = 1;
        if (rows < 1) rows = 1;
        site_resize_log(sel, cols, rows);

        if (sel->term != u->log_cache_term) {
            u->log_cache.all_rows_dirty = true;
            u->log_cache_term = sel->term;
        }
        /* A live server streams into this terminal while the panel sits open.
         * With the modal static (no open/close animation), the grid origin
         * never moves, so the cache would otherwise only do a full rebuild on
         * the rebind above — incremental per-row dirty replay was leaving the
         * first burst of a freshly-started server invisible until the panel
         * was reopened (whose animation shifted the origin and forced a
         * rebuild). The log grid is tiny, so just rebuild it every frame while
         * the process is alive. */
        if (sel->status == SITE_STARTING || sel->status == SITE_RUNNING)
            u->log_cache.all_rows_dirty = true;
        render_terminal_pane(app, sel->term, &u->log_cache, log_in_x, grid_y, false);
    } else if (sel && sel->term && !u->active) {
        /* Closing: panel background fades; the terminal is intentionally blank. */
    } else {
        /* Empty / onboarding state. */
        const char *title = (app->site_mgr.count == 0)
                            ? "No dev servers yet" : "Stopped";
        const char *sub = (app->site_mgr.count == 0)
                          ? "Add a project folder to get started"
                          : "Press Start to run this site";
        f32 cx = log_in_x + log_in_w * 0.5f;
        f32 cy = log_in_y + log_in_h * 0.45f;
        f32 tw = (f32)s_strchars(title) * ui_cw;
        f32 sw = (f32)s_strchars(sub) * ui_cw;
        s_text(r, title, cx - tw * 0.5f, cy, text_fg, ui_cw, -1);
        s_text(r, sub, cx - sw * 0.5f, cy + ui_ch + 6.0f * dpi, text_dim, ui_cw, -1);
        renderer_flush_glyphs(r);
    }

    renderer_reset_ui_scale(r);
}
